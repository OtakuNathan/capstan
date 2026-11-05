import json,shlex,subprocess,shutil
from pathlib import Path
ROOT=Path.cwd()
def compile_link(build,source,name,role,transport):
    variables={}
    for line in (build/f'CMakeFiles/capstan_{role}.dir/flags.make').read_text().splitlines():
        if ' = ' in line:
            k,v=line.split(' = ',1);variables[k]=shlex.split(v)
    obj=ROOT/(name+'.o')
    commands=[['/usr/bin/g++',*variables['CXX_DEFINES'],*variables['CXX_INCLUDES'],*variables['CXX_FLAGS'],'-c',str(source),'-o',str(obj)]]
    link=shlex.split((build/f'CMakeFiles/capstan_{role}.dir/link.txt').read_text())
    link=[str(transport) if token=='libcapstan_gio.a' else token for token in link]
    commands.append([str(obj) if item==f'CMakeFiles/capstan_{role}.dir/src/apps/{role}.cpp.o' else str(ROOT/'build'/name) if item==f'capstan_{role}' else item for item in link if not item.startswith('-Wl,--dependency-file=')])
    for command in commands:
        result=subprocess.run(command,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        records.append(dict(command=command,exit_code=result.returncode,output=result.stdout))
        (ROOT/'reference-builds.json').write_text(json.dumps(records,indent=2)+'\n')
        if result.returncode:raise RuntimeError(result.stdout)
records=[]
reference=ROOT/'reference';reference.mkdir(exist_ok=True)
old_apps=reference/'unguarded-apps'
shutil.copytree(ROOT/'src/apps',old_apps,dirs_exist_ok=True)
p=old_apps/'authority_replies.hpp';p.write_text(p.read_text().replace('        if(!size_) return;\n',''))
s=(ROOT/'src/transport/gio.cpp').read_text().replace('#include <glib-unix.h>\n','')
s=s.replace('g_unix_fd_source_new(g_socket_get_fd(socket),static_cast<GIOCondition>(G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL))','g_socket_create_source(socket,static_cast<GIOCondition>(G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL),nullptr)')
s=s.replace('g_unix_fd_source_new(g_socket_get_fd(socket),static_cast<GIOCondition>(G_IO_OUT|G_IO_ERR|G_IO_HUP|G_IO_NVAL))','g_socket_create_source(socket,static_cast<GIOCondition>(G_IO_OUT|G_IO_ERR|G_IO_HUP|G_IO_NVAL),nullptr)')
s=s.replace('g_unix_fd_source_new(g_socket_get_fd(state_->listener),static_cast<GIOCondition>(G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL))','g_socket_create_source(state_->listener,static_cast<GIOCondition>(G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL),nullptr)')
s=s.replace('+[](gint,GIOCondition','+[](GSocket*,GIOCondition')
variables={}
for line in (ROOT/'build/CMakeFiles/capstan_gio.dir/flags.make').read_text().splitlines():
    if ' = ' in line:
        k,v=line.split(' = ',1);variables[k]=shlex.split(v)
for policy,code in [('cleared',s),('inherited',s.replace('        g_socket_set_timeout(socket,0);\n',''))]:
    source=reference/(policy+'.cpp');source.write_text(code)
    transport=reference/(policy+'.o')
    command=['/usr/bin/g++',*variables['CXX_DEFINES'],*variables['CXX_INCLUDES'],*variables['CXX_FLAGS'],'-c',str(source),'-o',str(transport)]
    r=subprocess.run(command,cwd=ROOT/'build',stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    records.append(dict(command=command,exit_code=r.returncode,output=r.stdout))
    if r.returncode:raise RuntimeError(r.stdout)
    suffix='_reference' if policy=='cleared' else '_inherited'
    compile_link(ROOT/'build',old_apps/'host.cpp','capstan_host'+suffix,'host',transport)
    compile_link(ROOT/'build',ROOT/'src/apps/throughput.cpp','capstan_throughput'+suffix,'host',transport)
print('Built GSocketSource controls with cleared/inherited socket timeout; identical callback publication.')

compile_link(ROOT/'build',old_apps/'host.cpp','capstan_host_unguarded','host',ROOT/'build/libcapstan_gio.a')
