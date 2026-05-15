# boredos-axal

me larping as a kernel developer

this is a "performance patch" for boredos. i say "performance" because i have zero benchmarks and im mostly just vibing with what sounds fast in my head. added some stuff, changed some stuff, it compiles, it links, it boots. thats the bar and we cleared it

## what i did (the larping)

- replaced memcpy/memset with `rep movsb`/`rep stosb` because i read somewhere that modern cpus are good at that
- made syscall return use `sysret` instead of `iretq` when possible. osdev wiki said its faster so i trust them
- wrote a buddy allocator for physical pages. do i fully understand buddy allocators? mostly. probably. lets move on
- per-cpu magazine caches for small allocations. sounds cool right
- slapped an lru block cache on disk reads. 256 sectors. its not much but its honest work
- added tlb shootdown ipis for smp because that seemed important
- bumped compiler target to x86-64-v2 for the extra instructions

## install

```bash
./axal-installer install /path/to/your/BoredOS
```

backs up original files first.

## rollback

```bash
./axal-installer rollback /path/to/your/BoredOS
```

puts everything back. like i was never here

## build installer from source

```bash
gcc -O2 -static -o axal-installer axal-installer.c
```

## does it work

it boots. thats all i can promise. the rest is larping

## license

gpl v3 same as boredos
