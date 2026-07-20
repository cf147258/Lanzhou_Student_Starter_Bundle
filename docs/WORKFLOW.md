# Five-day integration workflow

## Daily branch flow

1. Start from the previous class-green tag.
2. Run the previous cumulative gate without editing code.
3. Reproduce today's fixed failure and preserve its raw signature.
4. Each group edits only its bounded TODOs and owned module.
5. Land small owner-approved commits into the shared integration branch at the
   scheduled minute marks.
6. Run `make check DAY=N TEAM=Gx` on the latest shared branch. If it is waiting
   on another owner, record that dependency rather than editing their module.
7. After all five owner commits land, run `make verify-dayN`; do not skip an
   earlier red gate.
8. Save evidence, name an owner and known issue, then create the day tag.
9. After the tag is green, optionally export a class checkpoint.

## Local and class gates

`make check` selects one group boundary inside one day. It is fast integration
feedback for the 22-minute build sprint, so it can expose an unmet upstream
contract as well as a defect in the selected group's module. It is not a claim
that one module can reproduce the entire robot path alone. `make verify-dayN`
runs G1 through G5 and every earlier day. Only the cumulative command
authorizes a class tag.

The module boundary keeps merge work small:

- G1 owns state and calibration.
- G2 owns frame and transport.
- G3 owns readiness and runtime.
- G4 owns arbitration and safety.
- G5 owns controller containment and the gatekeeper.

Cross-group review may propose a patch, but the owning group approves it before
it lands. Frozen public declarations change only through whole-class review
because all ten chapters rely on them.

## Expected initial result

The starter compiles and its smoke test is green. Daily tests are deliberately
red at the first bounded TODO. Stubs reject or discard; they never approve an
unsafe write. A crash, hang, missing tool, broker exposure, or compiler error is
an infrastructure defect rather than a student exercise.
