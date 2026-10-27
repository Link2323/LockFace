package internal

import (
	_ "embed"
	"encoding/json"
	"sync/atomic"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

//go:embed ui_messages.json
var uiMessagesJSON []byte

var uiMessages map[string]string
var uiLanguageMode atomic.Value
var getUserDefaultUILanguage = windows.NewLazySystemDLL("kernel32.dll").NewProc("GetUserDefaultUILanguage")

func init() {
	if err := json.Unmarshal(uiMessagesJSON, &uiMessages); err != nil {
		panic(err) // Invalid embedded catalog is a build-time programming error.
	}
	mode := ReadRegString("UiLanguage", "system")
	if !validLanguageMode(mode) {
		mode = "system"
	}
	uiLanguageMode.Store(mode)
}

func validLanguageMode(mode string) bool {
	return mode == "system" || mode == "zh-CN" || mode == "en"
}

func LanguageMode() string { return uiLanguageMode.Load().(string) }

func ResolveUILanguage(mode string, windowsLanguage uint16) string {
	if mode == "zh-CN" || (mode != "en" && windowsLanguage&0x3ff == 4) {
		return "zh-CN"
	}
	return "en"
}

func UILanguage() string {
	lang, _, _ := getUserDefaultUILanguage.Call()
	return ResolveUILanguage(LanguageMode(), uint16(lang))
}

// The same protected machine setting is used by the console and LogonUI.
// Keep a process snapshot so uninstall can delete registry keys without
// changing the language of its remaining progress/result messages.
func SetUILanguageMode(mode string) bool {
	if !validLanguageMode(mode) {
		return false
	}
	key, _, err := registry.CreateKey(registry.LOCAL_MACHINE, `SOFTWARE\LockFace`, registry.SET_VALUE)
	if err != nil {
		return false
	}
	defer key.Close()
	if key.SetStringValue("UiLanguage", mode) != nil {
		return false
	}
	uiLanguageMode.Store(mode)
	return true
}

func UIText(source string) string {
	if UILanguage() == "en" {
		if text, ok := uiMessages[source]; ok {
			return text
		}
	}
	return source
}
