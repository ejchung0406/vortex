# WIND DOWN (binding, from the lead)

The user has dropped the priority of the upstream PRs. Stop now.
- Submit no new Slurm jobs. Pending `pr_*` jobs have been cancelled; running
  ones may finish.
- Commit what you have in a consistent state: a proper commit (builds, message
  in upstream style with the two trailers) or leave the worktree changes
  uncommitted and say exactly what is uncommitted. No `wip` commits.
- XLEN 32 is out of scope: write "verified at XLEN 64 only" in the note.
- Delete nothing: branches, worktrees and build copies stay.
- Update your PR note to the true state (what ran, what did not, review
  findings still open) and send your final report within 10 minutes. Tell any
  reviewer you spawned to stop and report what it has.
