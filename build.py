#!python -u

import os, sys
import datetime
import re
import glob
import tarfile
import subprocess
import shutil
import time

def next_build_number():
    try:
        file = open('.build_number', 'r')
        build_number = file.read()
        file.close()
    except IOError:
        build_number = '0'

    file = open('.build_number', 'w')
    file.write(str(int(build_number) + 1))
    file.close()

    return build_number


def get_expired_symbols(name, age = 30):
    path = os.path.join(os.environ['SYMBOL_SERVER'], '000Admin\\history.txt')

    try:
        file = open(path, 'r')
    except IOError:
        return []

    threshold = datetime.datetime.utcnow() - datetime.timedelta(days = age)

    expired = []

    for line in file:
        item = line.split(',')

        if (re.match('add', item[1])):
            id = item[0]
            date = item[3].split('/')
            time = item[4].split(':')
            tag = item[5].strip('"')

            age = datetime.datetime(year = int(date[2]),
                                    month = int(date[0]),
                                    day = int(date[1]),
                                    hour = int(time[0]),
                                    minute = int(time[1]),
                                    second = int(time[2]))
            if (tag == name and age < threshold):
                expired.append(id)

        elif (re.match('del', item[1])):
            id = item[2].rstrip()
            try:
                expired.remove(id)
            except ValueError:
                pass

    file.close()

    return expired


def get_configuration(release, debug):
    configuration = release

    if debug:
        configuration += ' Debug'
    else:
        configuration += ' Release'

    return configuration


def get_target_path(release, arch, debug, vs):
    configuration = get_configuration(release, debug)
    name = ''.join(configuration.split(' '))
    target = { 'x86': os.sep.join([name, 'Win32']), 'x64': os.sep.join([name, 'x64']) }
    target_path = os.sep.join([vs, target[arch]])

    return target_path


def shell(command, dir):
    print(dir)
    print(command)
    sys.stdout.flush()
    
    sub = subprocess.Popen(' '.join(command), cwd=dir,
                           stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)

    for line in sub.stdout:
        print(line.decode(sys.getdefaultencoding()).rstrip())

    sub.wait()

    return sub.returncode


def find(name, path):
    for root, dirs, files in os.walk(path):
        if name in files:
            return os.path.join(root, name)


class msbuild_failure(Exception):
    def __init__(self, value):
        self.value = value
    def __str__(self):
        return repr(self.value)


def msbuild(platform, configuration, target, file, args, dir):
    vcvarsall = find('vcvarsall.bat', os.environ['VS'])

    os.environ['MSBUILD_PLATFORM'] = platform
    os.environ['MSBUILD_CONFIGURATION'] = configuration
    os.environ['MSBUILD_TARGET'] = target
    os.environ['MSBUILD_FILE'] = file
    os.environ['MSBUILD_EXTRA'] = args
    os.environ['MSBUILD_VCVARSALL'] = vcvarsall

    bin = os.path.join(os.getcwd(), 'msbuild.bat')

    status = shell([bin], dir)

    if (status != 0):
        raise msbuild_failure(configuration)


def build_sln(name, release, arch, debug, vs):
    configuration = get_configuration(release, debug)

    if arch == 'x86':
        platform = 'Win32'
    elif arch == 'x64':
        platform = 'x64'

    msbuild(platform, configuration, 'Build', name + '.sln', '', vs)

def remove_timestamps(path):
    try:
        os.unlink(path + '.orig')
    except OSError:
        pass

    os.rename(path, path + '.orig')

    src = open(path + '.orig', 'r')
    dst = open(path, 'w')

    for line in src:
        if line.find('TimeStamp') == -1:
            dst.write(line)

    dst.close()
    src.close()

def run_sdv(name, dir, vs):
    release = { 'vs2015':'Windows 10',
                'vs2017':'Windows 10' }

    configuration = get_configuration(release[vs], False)
    platform = 'x64'

    msbuild(platform, configuration, 'Build', name + '.vcxproj',
            '', os.path.join(vs, name))

    msbuild(platform, configuration, 'sdv', name + '.vcxproj',
            '/p:Inputs="/clean"', os.path.join(vs, name))

    msbuild(platform, configuration, 'sdv', name + '.vcxproj',
            '/p:Inputs="/check:default.sdv /debug"', os.path.join(vs, name))

    path = [vs, name, 'sdv', 'SDV.DVL.xml']
    remove_timestamps(os.path.join(*path))

    msbuild(platform, configuration, 'dvl', name + '.vcxproj',
            '', os.path.join(vs, name))

    path = [vs, name, name + '.DVL.XML']
    shutil.copy(os.path.join(*path), dir)

    path = [vs, name, 'refine.sdv']
    if os.path.isfile(os.path.join(*path)):
        msbuild(platform, configuration, 'sdv', name + '.vcxproj',
                '/p:Inputs=/refine', os.path.join(vs, name))


def symstore_del(name, age):
    symstore_path = [os.environ['KIT'], 'Debuggers']
    if os.environ['PROCESSOR_ARCHITECTURE'] == 'x86':
        symstore_path.append('x86')
    else:
        symstore_path.append('x64')
    symstore_path.append('symstore.exe')

    symstore = os.path.join(*symstore_path)

    for id in get_expired_symbols(name, age):
        command=['"' + symstore + '"']
        command.append('del')
        command.append('/i')
        command.append(str(id))
        command.append('/s')
        command.append(os.environ['SYMBOL_SERVER'])

        shell(command, None)


def symstore_add(name, release, arch, debug, vs):
    target_path = get_target_path(release, arch, debug, vs)

    symstore_path = [os.environ['KIT'], 'Debuggers']
    if os.environ['PROCESSOR_ARCHITECTURE'] == 'x86':
        symstore_path.append('x86')
    else:
        symstore_path.append('x64')
    symstore_path.append('symstore.exe')

    symstore = os.path.join(*symstore_path)

    version = '.'.join([os.environ['MAJOR_VERSION'],
                        os.environ['MINOR_VERSION'],
                        os.environ['MICRO_VERSION'],
                        os.environ['BUILD_NUMBER']])

    command=['"' + symstore + '"']
    command.append('add')
    command.append('/s')
    command.append(os.environ['SYMBOL_SERVER'])
    command.append('/r')
    command.append('/f')
    command.append('*.pdb')
    command.append('/t')
    command.append(name)
    command.append('/v')
    command.append(version)

    shell(command, target_path)


def manifest():
    cmd = ['git', 'ls-tree', '-r', '--name-only', 'HEAD']

    sub = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    output = sub.communicate()[0]
    ret = sub.returncode

    if ret != 0:
        raise(Exception("Error %d in : %s" % (ret, cmd)))

    return output.decode('utf-8')


def archive(filename, files, tgz=False):
    print(filename)
    access='w'
    if tgz:
        access='w:gz'
    tar = tarfile.open(filename, access)
    for name in files :
        try:
            tar.add(name)
        except:
            pass
    tar.close()


def getVsVersion():
    vsenv = {}
    vcvarsall= find('vcvarsall.bat', os.environ['VS'])

    vars = subprocess.check_output([vcvarsall, 'x86_amd64', '&&', 'set'], shell=True)

    for var in vars.splitlines():
        k, _, v = map(str.strip, var.strip().decode('utf-8').partition('='))
        if k.startswith('?'):
            continue
        vsenv[k] = v

    mapping = { '14.0':'vs2015',
                '15.0':'vs2017'}

    return mapping[vsenv['VisualStudioVersion']]


def main():
    debug = { 'checked': True, 'free': False }
    sdv = { 'nosdv': False, None: True }
    driver = 'xenbus'
    vs = getVsVersion()
    now = datetime.datetime.now()

    if 'VENDOR_NAME' not in os.environ.keys():
        os.environ['VENDOR_NAME'] = 'Xen Project'

    if 'VENDOR_PREFIX' not in os.environ.keys():
        os.environ['VENDOR_PREFIX'] = 'XP'

    if 'PRODUCT_NAME' not in os.environ.keys():
        os.environ['PRODUCT_NAME'] = 'Xen'

    os.environ['MAJOR_VERSION'] = '9'
    os.environ['MINOR_VERSION'] = '0'
    os.environ['MICRO_VERSION'] = '0'

    if 'BUILD_NUMBER' not in os.environ.keys():
        os.environ['BUILD_NUMBER'] = next_build_number()

    if 'GIT_REVISION' in os.environ.keys():
        revision = open('revision', 'w')
        print(os.environ['GIT_REVISION'], file=revision)
        revision.close()

    symstore_del(driver, 30)

    release = { 'vs2015':'Windows 8',
                'vs2017':'Windows 8' }

    shutil.rmtree(driver, ignore_errors=True)

    build_sln(driver, release[vs], 'x86', debug[sys.argv[1]], vs)

    build_sln(driver, release[vs], 'x64', debug[sys.argv[1]], vs)

    symstore_add(driver, release[vs], 'x86', debug[sys.argv[1]], vs)
    symstore_add(driver, release[vs], 'x64', debug[sys.argv[1]], vs)

    if len(sys.argv) <= 2 or sdv[sys.argv[2]]:
        run_sdv('xen', driver, vs)
        run_sdv('xenfilt', driver, vs)
        run_sdv('xenbus', driver, vs)

    archive(driver + '\\source.tgz', manifest().splitlines(), tgz=True)
    archive(driver + '.tar', [driver,'revision'])

if __name__ == '__main__':
    main()
