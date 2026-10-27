package internal

import (
	"errors"
	"fmt"
	"time"

	"golang.org/x/sys/windows/registry"
)

// ConsentVersion is the version of docs/legal/privacy-notice.txt the installer
// requires the installer to acknowledge. This is not biometric consent.
// consent_test.go pins it to the canonical text so
// the recorded version cannot drift from what the installer displayed.
const ConsentVersion = "1.2"

const (
	// Registry values written under HKLM\SOFTWARE\LockFace. The key is
	// ACL-locked by SetRegistryKeyACL before RecordConsent runs, so a
	// non-admin process cannot forge or clear the acceptance record.
	RegConsentVersion = "ConsentVersion"
	RegConsentTime    = "ConsentTime"
)

// RecordConsent stores the installation notice acknowledgment and local time.
// Historical registry names remain unchanged; individual biometric consent is
// recorded separately by the console under HKCU. No biometric data is stored
// here, but account-linked acknowledgment records may still be personal data.
func RecordConsent() error {
	if err := WriteRegString(RegConsentVersion, ConsentVersion); err != nil {
		return fmt.Errorf("write consent version: %w", err)
	}
	if err := WriteRegString(RegConsentTime, time.Now().Format(time.RFC3339)); err != nil {
		return fmt.Errorf("write consent time: %w", err)
	}
	return nil
}

// DeletePerUserConsentRecords removes the per-user acceptance records the
// console's consent gate stores under HKCU\SOFTWARE\LockFace (version +
// time, nothing else). PRIVACY.txt 7.6 describes best-effort uninstall
// cleanup, so uninstall deletes that key for the current
// user and for every profile hive currently mounted under HKEY_USERS; the
// pre-rename SOFTWARE\FaceLogin key is swept too. Best effort: hives of
// profiles that are not logged on are not mounted and are left alone; a hive
// that refuses deletion does not stop the sweep.
func DeletePerUserConsentRecords() error {
	keyNames := []string{`SOFTWARE\LockFace`, `SOFTWARE\FaceLogin`}
	var errs []error
	for _, name := range keyNames {
		if err := deleteRegKeyTree(registry.CURRENT_USER, name); err != nil {
			errs = append(errs, fmt.Errorf("current user %s: %w", name, err))
		}
	}
	sids := []string{}
	if users, err := registry.OpenKey(registry.USERS, "", registry.ENUMERATE_SUB_KEYS); err != nil {
		errs = append(errs, fmt.Errorf("enumerate HKEY_USERS: %w", err))
	} else {
		names, err := users.ReadSubKeyNames(-1)
		users.Close()
		if err != nil {
			errs = append(errs, fmt.Errorf("enumerate HKEY_USERS: %w", err))
		} else {
			sids = names
		}
	}
	for _, sid := range sids {
		for _, name := range keyNames {
			if err := deleteRegKeyTree(registry.USERS, sid+`\`+name); err != nil {
				errs = append(errs, fmt.Errorf("hive %s %s: %w", sid, name, err))
			}
		}
	}
	return errors.Join(errs...)
}
