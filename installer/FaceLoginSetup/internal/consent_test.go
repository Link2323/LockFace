package internal

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The installer records ConsentVersion, and the privacy notice shown to the
// user carries its own version line. If the text is revised without bumping
// the constant (or vice versa), the recorded consent would point at a
// different document than the one the user actually read.
func TestConsentVersionMatchesNotice(t *testing.T) {
	path := filepath.Join("..", "..", "..", "docs", "legal", "privacy-notice.txt")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read canonical privacy notice: %v", err)
	}
	text := string(data)

	versionLine := regexp.MustCompile(`版本：([0-9]+\.[0-9]+)`).FindStringSubmatch(text)
	if versionLine == nil {
		t.Fatal("privacy notice has no 版本：x.y line")
	}
	if versionLine[1] != ConsentVersion {
		t.Errorf("privacy notice version %q != ConsentVersion %q", versionLine[1], ConsentVersion)
	}

	// Keep the user-facing promises that the consent gate relies on pinned to
	// the canonical text, so a future edit cannot silently remove them.
	for _, phrase := range []string{
		"不上传人脸信息",
		"不保存人脸照片、视频或者摄像头原始帧",
		"并非唯一验证方式",
		"无法保证绝对安全",
	} {
		if !strings.Contains(text, phrase) {
			t.Errorf("privacy notice is missing required phrase: %s", phrase)
		}
	}
}
