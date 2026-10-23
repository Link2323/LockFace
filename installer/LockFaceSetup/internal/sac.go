package internal

import (
	"encoding/xml"
	"strings"
	"time"
)

// Two external blockers can terminate LockFaceService.exe during startup
// before it writes a single log line — the installer then only sees
// "start service: 1053" (upstream FaceLogin #25):
//
//   - Smart App Control / WDAC code integrity killing the process while it
//     loads the unsigned payload DLLs (libprotobuf.dll and friends);
//   - antivirus quarantining or blocking a payload file (Windows Defender
//     leaves a detectable trail; third-party AV does not).
//
// After a failed service start, query the corresponding event channels for
// recent events naming one of our payload binaries so the user sees the real
// cause and remedy instead of the generic 1053.
const (
	codeIntegrityChannel = "Microsoft-Windows-CodeIntegrity/Operational"
	// 3076/3077 = code integrity validation failure (audit / block). SAC
	// suppressions of unsigned DLLs land here.
	codeIntegrityQuery = "*[System[(EventID=3076 or EventID=3077)]]"
	defenderChannel    = "Microsoft-Windows-Windows-Defender/Operational"
	// 1116 = malware detected, 1117 = action taken. The affected path is a
	// named "Path" EventData field in drive-letter form.
	defenderQuery = "*[System[(EventID=1116 or EventID=1117)]]"
	// Only events this fresh are considered — the service start just failed,
	// so a block that explains it must be recent. Stale events from an
	// earlier attempt must not turn unrelated start failures into blocker
	// advice.
	blockDetectionWindow = 10 * time.Minute
)

type ciEventLog struct {
	Event []ciEvent `xml:"Event"`
}

type ciEvent struct {
	System struct {
		TimeCreated struct {
			SystemTime string `xml:"SystemTime,attr"`
		} `xml:"TimeCreated"`
	} `xml:"System"`
	EventData struct {
		Data []ciEventData `xml:"Data"`
	} `xml:"EventData"`
}

type ciEventData struct {
	Name  string `xml:"Name,attr"`
	Value string `xml:",chardata"`
}

// blockedPayloadFile returns the payload file named in a recent blocker
// event, or "" when nothing matches. Matching is on the file name only:
// CodeIntegrity data carries the path in \Device\ form and Defender data in
// drive-letter form — neither ever matches the user-chosen install directory
// string, but both end in the file name.
func blockedPayloadFile(xmlText string, now time.Time, rootFiles []string) string {
	xmlText = strings.TrimPrefix(xmlText, "\ufeff")
	var log ciEventLog
	if err := xml.Unmarshal([]byte(xmlText), &log); err != nil {
		return ""
	}
	for _, ev := range log.Event {
		stamp, err := time.Parse(time.RFC3339, ev.System.TimeCreated.SystemTime)
		if err != nil || stamp.After(now.Add(time.Minute)) ||
			now.Sub(stamp) > blockDetectionWindow {
			continue
		}
		for _, d := range ev.EventData.Data {
			for _, file := range rootFiles {
				if strings.HasSuffix(strings.ToLower(d.Value), strings.ToLower(file)) {
					return file
				}
			}
		}
	}
	return ""
}

// recentBlockOnPayload runs one wevtutil query against an event channel and
// reports the payload file a recent event named. Best effort: any failure
// reading the channel (absent, disabled, wevtutil missing) yields "" so the
// caller's original error stays untouched.
func recentBlockOnPayload(channel, query string) string {
	out, _ := RunCommand("wevtutil", "qe", channel,
		"/q:"+query, "/c:16", "/rd:true", "/f:xml", "/e:true")
	return blockedPayloadFile(out, time.Now(), PayloadManifestRootFiles())
}

// DetectCodeIntegrityBlock reports which payload file (if any) a recent
// Smart App Control / code integrity validation failure blocked.
func DetectCodeIntegrityBlock() string {
	return recentBlockOnPayload(codeIntegrityChannel, codeIntegrityQuery)
}

// DetectDefenderBlock reports which payload file (if any) a recent Windows
// Defender detection (quarantine / block) named. Third-party antivirus has
// no central event log — those cases fall through to the caller's generic
// start-failure hint.
func DetectDefenderBlock() string {
	return recentBlockOnPayload(defenderChannel, defenderQuery)
}
