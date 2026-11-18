# Security policy

NeonSynth is a plugin: it runs inside your DAW's process and reads files you give it. A bug that
lets a file crash or hang the plugin can take the whole host, and your unsaved project, with it.
Reports of such bugs are welcome.

## Supported versions

| Version | Supported |
|---|---|
| 1.10.x | Yes: fixes are released as a new 1.10 patch release |
| Older than 1.10.0 | Best effort: the fix goes into the next release, not into old versions |

## Reporting a vulnerability

Please report privately, not in a public issue or pull request.

1. Open the repository's **[Security](https://github.com/kv244/NeonSynth/security)** tab.
2. Choose **Report a vulnerability**. This opens a private advisory that only you and the maintainer
   can see.

If the button is not there, open a public issue that says only that you have a security report and
asks for a private channel. Leave out the details and the file until then.

## What to include

- The NeonSynth version (from the installer's name, `NeonSynth-x.y.z-Windows-Setup.exe`, or the
  release you built from) and how you run it: the VST3 in which host and version, or the Standalone.
- The operating system and its version.
- What happens: a crash, a hang, a very long stall, memory that keeps growing, or something else.
- The steps to reproduce it, as few as you can.
- The file that triggers it, if there is one: a saved session or preset, a sample file, or the
  plugin state as the host stores it. Attach it to the private advisory, not to a public issue.
- A crash dump or stack trace, if your host or system wrote one.

## What counts

These are in scope:

- A **session or state file** that crashes or hangs the plugin when the host loads it. The state
  holds the parameters, the sequencer and CHOP patterns and, often, the sample as embedded FLAC, so a
  malformed or oversized state is the most likely attack.
- A **sample file** (WAV, AIFF or FLAC) that crashes or hangs the plugin when you load it, or when a
  session that names it is opened.
- MIDI input that crashes or hangs the plugin.
- Anything that makes the plugin read or write files other than the sample you chose and the MIDI log
  you turned on with `NEONSYNTH_MIDILOG`.
- A problem in the Windows installer, such as files written outside the folders it lists.

These are not security issues; please open a normal issue instead:

- A wrong sound, a click or a glitch, unless a file causes it on purpose and it harms the host.
- High CPU use from settings you chose (for example 16 granular voices at full density).
- Bugs in the host or in JUCE itself. Report those to their own projects; tell us too if NeonSynth
  needs to work around them.

## What happens next

The maintainer works on NeonSynth in spare time, so there is no fixed response time. You will get a
reply in the advisory, and the fix is released as a normal release whose
[changelog](CHANGELOG.md) entry mentions it. You are credited in the advisory unless you ask not to be.
