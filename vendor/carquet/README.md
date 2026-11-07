# Carquet

Zen Table links Carquet 0.7.3 from commit
`2e3e5c3a4c7de3341e448ddc04c47dfebbe19d99`.
The source archive SHA-256 is
`c4d0077170b64966429aaf8d194095008191b8730c5150185fc6fe884adc5d6d`.

The build verifies and extracts the archive into `build/carquet-src`.
The library links to the distribution's zstd, zlib, LZ4 and Brotli libraries.
Its CMake wrapper disables dependency fetching and targets the host's normal CPU baseline.
The source archive includes the upstream tests and implementation.

Carquet is MIT licensed. Its Snappy implementation includes Google BSD-3-Clause code.
Both notices accompany the Zen Table package. [Upstream source](https://github.com/Vitruves/carquet/tree/2e3e5c3a4c7de3341e448ddc04c47dfebbe19d99)
