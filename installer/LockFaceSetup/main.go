package main

import (
	"embed"
	"fmt"
	"os"

	"LockFaceSetup/internal"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/windows"
)

//go:embed all:frontend/dist
var assets embed.FS

// appVersion is shown in Windows' Add/Remove Programs list. Keep in sync
// with wails.json Info.productVersion and build/windows/info.json.
const appVersion = "1.9.0"

// Registry value names under the LockFace key (the key path itself lives in
// internal/util.go).
const REGVAL_DATA_PATH = "DataPath"
const REGVAL_INSTALL_PATH = "InstallPath"

// startupUninstall lands the UI on the uninstall page instead of install
// (full installer launched with --uninstall; the slim uninstaller build is
// always in uninstall mode via uninstallerBuild).
var startupUninstall = false

func main() {
	for _, arg := range os.Args[1:] {
		if arg == "--uninstall" {
			startupUninstall = true
		}
	}

	// Check administrator — if not elevated, relaunch as admin
	if !internal.IsAdmin() {
		err := internal.Elevate()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Elevation failed: %v\n", err)
		}
		os.Exit(0)
	}

	// Initialize the embedded resource filesystem in the internal package.
	// In the slim build this is an empty FS — install is unavailable there.
	internal.EmbeddedFS = resources

	app := NewApp()

	title := internal.UIText("LockFace 安装程序")
	if uninstallerBuild {
		title = internal.UIText("LockFace 卸载程序")
	}

	// Paint the caption the same white as the page background — the LockFace
	// Console window does the same via DWMWA_CAPTION_COLOR (WebviewHost.cpp).
	// Theme is pinned to Light so the caption buttons stay dark-on-white even
	// on a dark-mode system. Colors are COLORREF (0x00BBGGRR); the border uses
	// DWMWA_COLOR_DEFAULT (-1) to keep the system edge.
	const (
		captionWhite  = int32(0xFFFFFF)
		captionText   = int32(0x000000)
		defaultBorder = int32(-1)
	)
	theme := windows.ThemeSettings{
		LightModeTitleBar:          captionWhite,
		LightModeTitleText:         captionText,
		LightModeBorder:            defaultBorder,
		LightModeTitleBarInactive:  captionWhite,
		LightModeTitleTextInactive: captionText,
		LightModeBorderInactive:    defaultBorder,
		DarkModeTitleBar:           captionWhite,
		DarkModeTitleText:          captionText,
		DarkModeBorder:             defaultBorder,
		DarkModeTitleBarInactive:   captionWhite,
		DarkModeTitleTextInactive:  captionText,
		DarkModeBorderInactive:     defaultBorder,
	}

	err := wails.Run(&options.App{
		Title:            title,
		Width:            640,
		Height:           600,
		MinWidth:         600,
		MinHeight:        560,
		WindowStartState: options.Normal,
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		BackgroundColour: &options.RGBA{R: 255, G: 255, B: 255, A: 1},
		Windows: &windows.Options{
			Theme:       windows.Light,
			CustomTheme: &theme,
		},
		OnStartup: app.startup,
		Bind: []interface{}{
			app,
		},
	})
	if err != nil {
		println("Error:", err.Error())
	}
}
