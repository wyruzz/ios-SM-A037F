# Publishing this repository

This directory is a **sanitised export** of the A03s port. Before pushing:

## 1. Review what is in it

```sh
cd $HOME/a03s-public
git init
git add -A
git status --short | head -50
git ls-files | wc -l
du -sh .git .
```

Expected: ~200 files, ~6 MB, no file above a few hundred KB.

## 2. Check for personal data and Apple files

```sh
git grep -n -E 'wyruzz|RDW00|/Users/|/home/|serial-number|ECID|macaddr' || true
git ls-files | grep -E '\.(ipsw|dmg|im4p|apfs|img|dylib|dsc|car)$' || true
```

The loader serial is a neutral placeholder (`HAKIOSA03S01`); replace it only in
a private build.

## 3. Create the repository on GitHub

There is no `gh` on this machine. Use the web UI:

1. <https://github.com/new>
2. Name, e.g. `a03s-hakios` (or whatever you prefer).
3. **Private** is a good default while the port is unfinished; make it public
   when you are ready.
4. Do **not** add a README/LICENSE/.gitignore from the web UI; this directory
   already has them.

## 4. Push

```sh
cd $HOME/a03s-public
git branch -M main
git remote add origin git@github.com:<your-account>/<repo>.git
git push -u origin main
```

(HTTPS works too: `https://github.com/<your-account>/<repo>.git`.)

## 5. After publishing

- Add the A03s status to the README when the touchscreen or SpringBoard stage
  changes.
- Keep `STATUS.md` current: it is the honest "what works / what does not" page.
- Never add `identity.h`, the IPSW, the dyld cache, `system.apfs`,
  `flashboot.apfs`, boot backups or logs containing a serial/ECID.
- The A03s kernel changes are described in `kernel/README.md`, not shipped. If
  you add them to a public tree, keep `LICENSE.APSL` and Apple's notices; better
  still, keep them in a private tree.
