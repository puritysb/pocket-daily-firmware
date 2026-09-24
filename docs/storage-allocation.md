# Fallible file-handle allocation

`HalStorage.cpp` previously used `std::make_unique<HalFile::Impl>` at four file
handle creation sites. The wrapper could fail before callers received a handle
to check, undermining content/transfer error handling on a constrained reader.

The existing one-Impl-per-handle allocation now uses `makeUniqueNoThrow` with a
null check and error log. Allocation occurs **before** SDK open/read-open/
write-open/directory-next calls. In particular, a wrapper allocation failure
does not invoke the SDK write-open routine or advance a directory cursor.
There is no permanent pool, additional per-file allocation or change to the
recursive storage mutex. `sizeof(Impl)` remains the allocation size; the object
must outlive the opening function and move with its owning HalFile.

Out-parameter open helpers return false and release their prior handle on OOM,
matching the replacement semantics of ordinary failed opens. That prior close
still runs under the storage lock and may flush prior work; the *new target*
is not opened. Empty/default/moved-from/OOM handles are safe to close/flush/
rewind/inspect. Reads return -1, writes return0, seeks/preallocation/rename fail,
and sizes/availability return0. Callers must still check the handle before
treating zero length as a valid empty file.

## Verification boundaries

`test/hal_storage` compiles the production `HalStorage.cpp`, substituting SDK,
mutex and allocation-helper stubs. Tests inject null allocation, count SDK
entry/cursor/close calls, check lock depth, exercise every empty-handle fallback,
and check normal/failed SDK opens. This differs from content-store tests, which
replace the entire HAL. Neither proves real SD concurrency, real malloc failure,
watchdog safety, throughput or filesystem durability. Other SDK and logging
allocations are outside this fix. No device was accessed or updated.
