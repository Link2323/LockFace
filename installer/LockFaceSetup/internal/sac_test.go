package internal

import (
	"strings"
	"testing"
	"time"
)

// wevtutil qe Microsoft-Windows-CodeIntegrity/Operational /f:xml /e:true
// shape. 3076/3077 EventData carries the validated path in \Device\ form —
// the matcher must hit on the file name, never on an install-dir string.
const ciSampleEvents = `<Events><Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
<System><Provider Name="Microsoft-Windows-CodeIntegrity"></Provider><EventID>3077</EventID>
<TimeCreated SystemTime="RECENT"></TimeCreated></System>
<EventData><Data>\Device\HarddiskVolume3\Program Files\LockFace\libprotobuf.dll</Data>
<Data>0xC0E90002</Data></EventData></Event>
<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
<System><EventID>3076</EventID>
<TimeCreated SystemTime="RECENT"></TimeCreated></System>
<EventData><Data>\Device\HarddiskVolume3\Program Files\SomeOtherApp\unsigned.dll</Data></EventData></Event>
<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
<System><EventID>3077</EventID>
<TimeCreated SystemTime="STALE"></TimeCreated></System>
<EventData><Data>\Device\HarddiskVolume3\Program Files\LockFace\onnxruntime.dll</Data></EventData></Event>
</Events>`

// Defender detections (1116/1117) carry the affected file in a NAMED
// "Path" EventData field in drive-letter form — the parser must capture
// named-field chardata too, not just unnamed Data elements.
const defenderSample = `<Events><Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
<System><EventID>1117</EventID>
<TimeCreated SystemTime="RECENT"></TimeCreated></System>
<EventData><Data Name="Threat Name">Trojan:Win32/FakeTest</Data>
<Data Name="Path">C:\Program Files\LockFace\onnxruntime.dll</Data>
<Data Name="Action Name">Quarantine</Data></EventData></Event>
<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
<System><EventID>1116</EventID>
<TimeCreated SystemTime="STALE"></TimeCreated></System>
<EventData><Data Name="Path">C:\Program Files\LockFace\re2.dll</Data></EventData></Event>
</Events>`

func TestBlockedPayloadFileDefenderShape(t *testing.T) {
	now := time.Date(2026, 10, 23, 23, 30, 0, 0, time.Local)
	recent := now.Add(-1 * time.Minute).UTC().Format(time.RFC3339)
	stale := now.Add(-30 * time.Minute).UTC().Format(time.RFC3339)
	xmlText := strings.NewReplacer("RECENT", recent, "STALE", stale).Replace(defenderSample)
	rootFiles := PayloadManifestRootFiles()

	if got := blockedPayloadFile(xmlText, now, rootFiles); got != "onnxruntime.dll" {
		t.Errorf("named Path field quarantine not reported, got %q", got)
	}
	if got := blockedPayloadFile(
		strings.Replace(xmlText, recent, stale, 1), now, rootFiles); got != "" {
		t.Errorf("stale Defender detection reported: %q", got)
	}
}

func TestBlockedPayloadFile(t *testing.T) {
	now := time.Date(2026, 10, 23, 22, 0, 0, 0, time.Local)
	recent := now.Add(-2 * time.Minute).UTC().Format(time.RFC3339)
	stale := now.Add(-30 * time.Minute).UTC().Format(time.RFC3339)
	xmlText := strings.NewReplacer("RECENT", recent, "STALE", stale).Replace(ciSampleEvents)
	rootFiles := PayloadManifestRootFiles()

	if got := blockedPayloadFile(xmlText, now, rootFiles); got != "libprotobuf.dll" {
		t.Errorf("recent block of libprotobuf.dll not reported, got %q", got)
	}

	// A block older than the freshness window must not fire — it belongs to
	// an earlier attempt and cannot explain this start failure.
	if got := blockedPayloadFile(
		strings.Replace(xmlText, recent, stale, 1), now, rootFiles); got != "" {
		t.Errorf("stale block reported as current: %q", got)
	}

	// Foreign files never match the payload allow-list.
	foreign := strings.Replace(xmlText, recent, stale, 1) // age out the two LockFace hits
	if got := blockedPayloadFile(foreign, now, rootFiles); got != "" {
		t.Errorf("foreign-only events reported: %q", got)
	}

	if got := blockedPayloadFile("wevtutil: channel not found", now, rootFiles); got != "" {
		t.Errorf("non-XML output reported a block: %q", got)
	}
	if got := blockedPayloadFile("", now, rootFiles); got != "" {
		t.Errorf("empty output reported a block: %q", got)
	}
}
