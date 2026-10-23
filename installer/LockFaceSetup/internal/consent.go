package internal

import (
	"fmt"
	"time"
)

// ConsentVersion is the version of docs/legal/privacy-notice.txt the installer
// requires the user to accept. consent_test.go pins it to the canonical text so
// the recorded version cannot drift from what the installer displayed.
const ConsentVersion = "1.0"

const (
	// Registry values written under HKLM\SOFTWARE\FaceLogin. The key is
	// ACL-locked by SetRegistryKeyACL before RecordConsent runs, so a
	// non-admin process cannot forge or clear the acceptance record.
	RegConsentVersion = "ConsentVersion"
	RegConsentTime    = "ConsentTime"
)

// RecordConsent stores the accepted privacy-notice version and the local
// timestamp of the acceptance. It intentionally contains no personal data.
func RecordConsent() error {
	if err := WriteRegString(RegConsentVersion, ConsentVersion); err != nil {
		return fmt.Errorf("write consent version: %w", err)
	}
	if err := WriteRegString(RegConsentTime, time.Now().Format(time.RFC3339)); err != nil {
		return fmt.Errorf("write consent time: %w", err)
	}
	return nil
}
