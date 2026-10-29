"""Insert after memory aliases and before the framework section script."""
from pathlib import Path

def configure(env):
    if env['PIOENV'] not in ('rtc_probe_bridge', 'rtc_probe_production'):
        raise RuntimeError('RTC arena must remain confined to explicit diagnostic environments')
    script = Path(env['PROJECT_DIR'])/'scripts/rtc_probe_arena.ld'
    flags = list(env['LINKFLAGS'])
    if '-Tscripts/rtc_probe_arena.ld' in flags:
        raise RuntimeError('Do not place RTC script in early build flags')
    sections = [i for i in range(len(flags)-1)
                if flags[i] == '-T' and Path(str(flags[i+1])).name == 'sections.ld']
    memory = [i for i in range(len(flags)-1)
              if flags[i] == '-T' and Path(str(flags[i+1])).name == 'memory.ld']
    if len(sections) != 1 or len(memory) != 1 or memory[0] >= sections[0]:
        raise RuntimeError('Unexpected framework linker script order; refuse RTC reservation')
    flags[sections[0]:sections[0]] = ['-T', str(script)]
    env.Replace(LINKFLAGS=flags)

Import('env')  # noqa: F821 -- provided by PlatformIO
configure(env)  # noqa: F821
