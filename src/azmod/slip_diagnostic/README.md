# Native Slip scalar diagnostic

Core startup activates this diagnostic only when
`/home/root/settings/azmod.slip-capture.on` exists. Remove the switch and restart
to disable it. The diagnostic runs independently of community mods, installs
one exact firmware callback hook through the core installer, and writes to
`/tmp/azmod-slip-capture.log`. The callback does bounded scalar capture before
forwarding the original and preserves errno and native unwinding. It never
claims a raw scalar establishes enabled state or a logical deck.

The recorder accepts at most 512 observations, retains the latest 128 records,
and drains on a bounded worker for at most thirty minutes. Failed binding or
installation is explicit in the diagnostic log. The code and original entry
remain process-owned after installation. No mod lifecycle owns a hook or worker.

`profiles.py` regenerates exact code identities from locally supplied original
firmware. The generated code addresses and hashes are public binding metadata;
firmware files themselves remain local. The entry shim specializes the existing
core register/unwind bridge and does not copy that implementation into the SDK.

Run `python3 -m unittest discover -s tests/unit -p 'test_slip_*.py' -v` from core.
Supply `AZ_FIRMWARE_ROOT` containing `EP147-1.30` and `EP147-2.00` for exact image
controls. Linux covers ELF discovery; AArch64 Linux additionally executes the
actual entry bridge. Host tests do not establish native Slip meaning.

The SDK's Slip example is a separate public SETTINGS subscriber with hooks
turned off. Emulator raw-capture tooling explicitly enables this core diagnostic
before its owned restart and reads this core log. A public mod is not a native
instrumentation container.
