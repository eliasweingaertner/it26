# Contract: File formats

## IT module, ITS sample, ITI instrument (read and write)

- Sample header flag byte (+12h): bit 0 present, bit 1 16-bit, **bit 2 stereo**, bit 3 compressed — as ITTECH.TXT, OpenMPT, Schism Tracker.
- Data at the sample pointer: Length frames of the left channel, then Length frames of the right channel (uncompressed, signed as Cvt says).
- Writing: stereo samples are always written **uncompressed** (flag bit 3 clear), even when IT214/IT215 compression is selected — as OpenMPT does; mono samples are compressed as chosen. The module header's "compatible with" version is unchanged.
- Reading compressed stereo (flag bits 2 and 3, from other writers): the left channel's compressed blocks, then the right channel's, each channel a fresh block sequence.
- Reading files from other trackers with this layout: stereo.
- IT 2.14 reading it26's files: plays the left channel (documented).

## WAV / AIFF

- In: a 2-channel file asks "Loading Stereo Sample: Stereo / Left / Right"; Stereo keeps both channels.
- Out (IT's save-as-WAV, feature 018 exports): a stereo sample is written as a 2-channel WAV (8-bit unsigned / 16-bit signed, interleaved).

## S3M (out)

- Each stereo sample is written as its mono mix ((L+R)/2); the warning screen lists "Stereo samples converted to mono" once.
