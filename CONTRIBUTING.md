# Git workflow

shaoDe uses three kinds of branches:

| Branch | Holds | Rule |
| --- | --- | --- |
| `master` | Tested work | Only updated from `develop` after the changes have been checked on real hardware. |
| `develop` | Finished but untested work | Work branches are merged here when they are ready to test. |
| `feat/<name>`, `fix/<name>`, `docs/<name>` | Work in progress | One branch per feature or fix, started from `develop`. |

Never commit new work directly to `master` or `develop`.

## Starting work

```sh
git switch develop
git switch -c feat/<name>      # or fix/<name>, docs/<name>
```

Use `git worktree add ../shaoDe-wt/<name> -b feat/<name> develop` to work on
several branches at once.

Commit in small, tested steps: the build and `ctest` should pass at each commit.
Mention the GitHub issue in the commit or merge message when there is one.

## Ready to test

When a branch builds, passes `ctest`, and does what it should in a nested session,
merge it into `develop`:

```sh
git switch develop
git merge --no-ff feat/<name>
cmake --build build && ctest --test-dir build --output-on-failure
```

`--no-ff` keeps a merge commit, so each feature can be found (or reverted) as one
unit. If branches conflict, fix the conflict in the merge commit on `develop`.

Test `develop` on real hardware (`--session` on a TTY, multiple monitors, real
applications). Fixes found while testing go on a new branch from `develop`, or on
the original branch followed by another merge.

## Promoting to master

Once everything on `develop` has been tested:

```sh
git switch master
git merge --ff-only develop
git push origin master develop
```

If only part of `develop` is ready, leave it there until the rest is tested or
revert the untested merge on `develop` first; do not cherry-pick around it.

Delete finished work branches after they reach `master`:

```sh
git branch -d feat/<name>
git worktree remove ../shaoDe-wt/<name>   # if one was used
```

## Keeping private details out

`origin` (github.com/Sondre234/shaoDe) is public. Do not commit LAN addresses,
host keys, passwords, or machine-specific access details.
