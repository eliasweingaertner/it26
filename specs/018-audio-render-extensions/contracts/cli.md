# Contract: ited batch options

```
ited --render-patterns <module> [--pattern N]
ited --dump-samples <module>
```

- Run without opening a window or an audio device; read `ited.cfg` for the
  Quicksave folder (render folder rules as interactive).
- `--render-patterns`: every pattern 0..199 holding data (or only N with
  `--pattern N`, 0..199) is rendered as a WAV file (Shift-F5 rate from
  `ited.cfg`, 32-bit float), named `<PFX><NNNN>.WAV` (counter from 0001).
- `--dump-samples`: every non-empty sample as `HHMMSSnn.WAV`.
- One line per file on stdout: `wrote <path>`; errors on stderr.
- Exit status: 0 when everything was written; 1 when the module can't be
  loaded, the folder is invalid, a write fails, or N is out of range.
- `-r <hz>` still sets the mixing rate.
