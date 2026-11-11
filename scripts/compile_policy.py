"""Use the selected build's exception policy for out-of-build compile probes."""
from pathlib import Path

def flags(build):
    cache=(Path(build)/'CMakeCache.txt').read_text()
    if 'CAPSTAN_NO_EXCEPTIONS:BOOL=ON' not in cache:
        return []
    strict='1' if 'CAPSTAN_NO_EXCEPTION_STRICT:BOOL=ON' in cache else '0'
    return ['-fno-exceptions','-DCAPSTAN_NO_EXCEPTION_STRICT='+strict,
        '-DFLUX_FOUNDRY_NO_EXCEPTION_STRICT='+strict,'-DPORTAL_NO_EXCEPTION_STRICT='+strict]
