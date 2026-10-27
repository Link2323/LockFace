package internal

import (
	"fmt"
	"syscall"
	"time"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc/mgr"
)

const SERVICE_NAME = "LockFaceService"

// ServiceExists checks if the named service is installed.
func ServiceExists(name string) (bool, error) {
	m, err := mgr.Connect()
	if err != nil {
		return false, fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	s, err := m.OpenService(name)
	if err != nil {
		if err == windows.ERROR_SERVICE_DOES_NOT_EXIST {
			return false, nil
		}
		// A service marked for deletion is still present until all handles are
		// closed and the SCM completes the asynchronous removal.
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			return true, nil
		}
		return false, fmt.Errorf("open service %q: %w", name, err)
	}
	s.Close()
	return true, nil
}

// stopService kills the service process and waits for SCM to observe STOPPED.
// LockFaceService does not handle a graceful SCM stop request, so the
// process is terminated directly. The wait closes the race between process
// exit and the SCM state update.
func stopService(s *mgr.Service) error {
	if s == nil {
		return nil
	}

	_, _ = RunCommand("taskkill", "/f", "/im", "LockFaceService.exe")

	for i := 0; i < 10; i++ {
		time.Sleep(500 * time.Millisecond)
		status, err := s.Query()
		if err != nil {
			if err == windows.ERROR_SERVICE_DOES_NOT_EXIST ||
				err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
				return nil
			}
			return fmt.Errorf("query service status: %w", err)
		}
		if status.State == windows.SERVICE_STOPPED {
			return nil
		}
	}

	// The process was killed; SCM can still be slow to publish STOPPED. The
	// caller may proceed to Delete and handle the marked-for-delete result.
	return nil
}

// waitForServiceGone waits for SCM to finish an asynchronous DeleteService.
func waitForServiceGone(name string) error {
	for i := 0; i < 20; i++ {
		exists, err := ServiceExists(name)
		if err != nil {
			return err
		}
		if !exists {
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return fmt.Errorf("service %q is still present after waiting for deletion", name)
}

// StopAndDeleteService stops (if running) and deletes the service.
func StopAndDeleteService() error {
	exists, err := ServiceExists(SERVICE_NAME)
	if err != nil {
		return err
	}
	if !exists {
		return nil
	}

	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// Open the service once, stop it, then delete it. Opening first lets us
	// query SCM state instead of guessing with a fixed sleep.
	s, err := m.OpenService(SERVICE_NAME)
	if err != nil {
		if err == windows.ERROR_SERVICE_DOES_NOT_EXIST {
			return nil
		}
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			return waitForServiceGone(SERVICE_NAME)
		}
		return fmt.Errorf("open for delete: %w", err)
	}

	stopErr := stopService(s)
	err = s.Delete()
	s.Close()
	if err != nil {
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			if waitErr := waitForServiceGone(SERVICE_NAME); waitErr != nil {
				return fmt.Errorf("service marked for deletion but did not disappear: %w", waitErr)
			}
			return nil
		}
		if stopErr != nil {
			return fmt.Errorf("delete service after stop failed: %v: %w", stopErr, err)
		}
		return fmt.Errorf("delete service: %w", err)
	}

	return waitForServiceGone(SERVICE_NAME)
}

func createService(m *mgr.Mgr, exePath string) (*mgr.Service, error) {
	return m.CreateService(
		SERVICE_NAME,
		exePath,
		mgr.Config{
			StartType:    mgr.StartAutomatic,
			ErrorControl: mgr.ErrorNormal,
			DisplayName:  UIText("LockFace 人脸认证服务"),
			Description:  UIText("LockFace — 基于人脸识别的 Windows 登录认证服务"),
			ServiceType:  windows.SERVICE_WIN32_OWN_PROCESS,
		},
	)
}

// configureServiceRecovery arms SCM failure recovery: a crashed or externally
// killed LockFaceService is restarted by the SCM (upstream FaceLogin #10 —
// without this the lock screen tile stays on "人脸登录服务不可用" until the
// machine reboots; the CP already reports the outage, it cannot heal it).
// The non-crash flag additionally counts clean exits with a nonzero code as
// failures; Windows applies that flag only after the SCM itself restarts,
// i.e. on the next boot — until then recovery covers crash-only failures.
func configureServiceRecovery(s *mgr.Service) error {
	actions := []mgr.RecoveryAction{
		{Type: mgr.ServiceRestart, Delay: 5 * time.Second},
		{Type: mgr.ServiceRestart, Delay: 5 * time.Second},
		{Type: mgr.ServiceRestart, Delay: time.Minute},
	}
	if err := s.SetRecoveryActions(actions, 86400); err != nil {
		return fmt.Errorf("set service recovery actions: %w", err)
	}
	if err := s.SetRecoveryActionsOnNonCrashFailures(true); err != nil {
		return fmt.Errorf("set recovery on non-crash failures: %w", err)
	}
	return nil
}

// InstallService creates (or updates) and starts the service.
func InstallService(exePath string) error {
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// Stop an existing service before replacing its executable. Keep the SCM
	// handle open while querying status, then close it before CreateService.
	if existing, openErr := m.OpenService(SERVICE_NAME); openErr == nil {
		if err := stopService(existing); err != nil {
			existing.Close()
			return fmt.Errorf("stop existing service: %w", err)
		}
		existing.Close()
	} else if openErr != windows.ERROR_SERVICE_DOES_NOT_EXIST {
		if openErr == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			if err := waitForServiceGone(SERVICE_NAME); err != nil {
				return err
			}
		} else {
			return fmt.Errorf("open existing service: %w", openErr)
		}
	}

	// Try to create. A service that disappeared asynchronously may still
	// return 1072 once; wait for the name to become reusable and retry once.
	s, err := createService(m, exePath)
	if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
		if waitErr := waitForServiceGone(SERVICE_NAME); waitErr != nil {
			return fmt.Errorf("wait for marked-for-delete service: %w", waitErr)
		}
		s, err = createService(m, exePath)
	}
	if err == windows.ERROR_SERVICE_EXISTS {
		// Service already exists — open and update config.
		s, err = m.OpenService(SERVICE_NAME)
		if err != nil {
			return fmt.Errorf("open existing service: %w", err)
		}
		cfg, cfgErr := s.Config()
		if cfgErr != nil {
			s.Close()
			return fmt.Errorf("get service config: %w", cfgErr)
		}
		// EscapeArg is required on the update path for install directories with
		// spaces; preserve this invariant when updating an existing service.
		cfg.BinaryPathName = syscall.EscapeArg(exePath)
		cfg.StartType = mgr.StartAutomatic
		if cfgErr = s.UpdateConfig(cfg); cfgErr != nil {
			s.Close()
			return fmt.Errorf("update service config: %w", cfgErr)
		}
	}
	if err != nil {
		return fmt.Errorf("create service: %w", err)
	}
	defer s.Close()

	// Fresh create and upgrade-over-existing both land here — arm recovery
	// on either path so an updated install heals a crashing service too.
	if err := configureServiceRecovery(s); err != nil {
		return fmt.Errorf("configure service recovery: %w", err)
	}

	// Start the service
	err = s.Start()
	if err != nil && err != windows.ERROR_SERVICE_ALREADY_RUNNING {
		// SAC / code integrity kills the process during DLL load before it
		// can log anything; surface the blocked file instead of bare 1053.
		if blocked := DetectCodeIntegrityBlock(); blocked != "" {
			return fmt.Errorf(UIText("start service: %w（%s 被 Smart App Control/代码完整性拦截，服务进程启动即被终止。请在 Windows 安全中心 → 应用和浏览器控制 → 智能应用控制 中选择\"关闭\"后重新运行安装程序）"), err, blocked)
		}
		// Antivirus quarantine of a payload file fails the start the same
		// way; Windows Defender at least leaves a detectable trail.
		if blocked := DetectDefenderBlock(); blocked != "" {
			return fmt.Errorf(UIText("start service: %w（%s 已被 Windows Defender 检测并隔离/拦截。请在 Windows 安全中心 → 病毒和威胁防护 → 保护历史记录 中恢复该文件并允许，或将安装目录加入排除项后重新运行安装程序）"), err, blocked)
		}
		// 1053 with zero service log lines means the process died before
		// main(); without an event trail the honest answer is the checklist.
		if err == windows.ERROR_SERVICE_REQUEST_TIMEOUT {
			return fmt.Errorf(UIText("start service: %w（服务进程启动后很快退出且未写任何日志。常见原因：Smart App Control 或杀毒软件拦截未签名程序、系统缺少 Visual C++ 运行库；处理办法见运维手册\"系统要求\"与\"常见问题\"）"), err)
		}
		return fmt.Errorf("start service: %w", err)
	}
	return nil
}
