package internal

import "testing"

func TestResolveUILanguage(t *testing.T) {
	for _, tc := range []struct {
		mode string
		lang uint16
		want string
	}{
		{"system", 0x0804, "zh-CN"},
		{"system", 0x0404, "zh-CN"},
		{"system", 0x0409, "en"},
		{"system", 0x0407, "en"},
		{"en", 0x0804, "en"},
		{"zh-CN", 0x0409, "zh-CN"},
	} {
		if got := ResolveUILanguage(tc.mode, tc.lang); got != tc.want {
			t.Errorf("ResolveUILanguage(%q, %x) = %q, want %q", tc.mode, tc.lang, got, tc.want)
		}
	}
}

func TestUITextKeepsSelectedLanguageAfterRegistryCleanup(t *testing.T) {
	previous := LanguageMode()
	t.Cleanup(func() { uiLanguageMode.Store(previous) })
	// No registry writes: uninstall removes the registry while this process
	// must continue presenting the selected language from its snapshot.
	uiLanguageMode.Store("en")
	if got := UIText("完成"); got != "Finished" {
		t.Fatalf("English completion = %q", got)
	}
	if got := UIText("C:\\Program Files\\LockFace"); got != "C:\\Program Files\\LockFace" {
		t.Fatalf("unknown data changed: %q", got)
	}
	uiLanguageMode.Store("zh-CN")
	if got := UIText("完成"); got != "完成" {
		t.Fatalf("Chinese completion = %q", got)
	}
}
