# Class-generated checkpoints

The student release contains no checkpoint archive. After a cumulative class
gate is green, `make checkpoint DAY=N GROUP=CLASS` may create one here from the
merged student modules and matching evidence.

`make restore CHECKPOINT=checkpoints/<file>.tar.gz` validates paths and hashes,
backs up the current student modules, and restores only allowed student and
evidence files. Use only an archive named by the instructor.
