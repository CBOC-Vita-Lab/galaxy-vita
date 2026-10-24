#!/usr/bin/env python3
"""Desktop installer. Includes Python, DolphinTool, the APK and adb.

Run from source: python tools/install.py (APK in out/GalaxyQuest.apk).
Build: python tools/build_installer.py --apk out/GalaxyQuest.apk --dolphin-tool PATH
"""
import multiprocessing

# Must run before importing Tk or parsing arguments: frozen conversion workers
# re-enter this executable with multiprocessing's private command-line flags.
if __name__ == '__main__':
    multiprocessing.freeze_support()

import argparse
import concurrent.futures
import contextlib
import datetime
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import traceback
import webbrowser

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'cook'))
sys.path.insert(0, str(HERE))
import cook
import push_data

DEVELOPER_HELP = 'https://developers.meta.com/horizon/documentation/android-apps/enable-developer-mode/'
GIB = 1024 ** 3
DISC_EXTENSIONS = {'.iso', '.rvz', '.wbfs', '.gcz', '.wia', '.ciso'}


class InstallError(Exception):
    pass


def assets_dir():
    return Path(getattr(sys, '_MEIPASS', HERE.parent / 'out')) / 'assets'


def cache_dir():
    if sys.platform == 'win32':
        base = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData' / 'Local'))
    elif sys.platform == 'darwin':
        base = Path.home() / 'Library' / 'Caches'
    else:
        base = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache'))
    return base / 'GalaxyQuest'


def find_adb():
    name = 'adb.exe' if sys.platform == 'win32' else 'adb'
    beside = Path(sys.executable).parent if getattr(sys, 'frozen', False) else HERE.parent
    for candidate in (assets_dir() / 'platform-tools' / name,
                      beside / 'platform-tools' / name):
        if candidate.is_file():
            return str(candidate)
    try:
        return push_data.find_adb()
    except SystemExit:
        raise InstallError('Connection tools are missing. Use a packaged GalaxyQuest installer, '
                           'or put the Android platform-tools folder next to this launcher.') from None


def find_apk():
    bundled = assets_dir() / 'GalaxyQuest.apk'
    beside = Path(sys.executable).parent if getattr(sys, 'frozen', False) else HERE.parent / 'out'
    for candidate in (bundled, beside / 'GalaxyQuest.apk'):
        if candidate.is_file():
            return candidate
    raise InstallError('GalaxyQuest.apk is missing. Use a packaged installer, '
                       'or put GalaxyQuest.apk in the out folder when running from source.')


def find_dolphin_tool():
    names = ('DolphinTool.exe',) if sys.platform == 'win32' else ('dolphin-tool', 'DolphinTool')
    beside = Path(sys.executable).parent if getattr(sys, 'frozen', False) else HERE.parent
    for directory in (assets_dir() / 'dolphin', beside / 'dolphin'):
        for name in names:
            candidate = directory / name
            if candidate.is_file():
                return str(candidate)
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise InstallError('The disc extraction tool is missing. Use the complete GalaxyQuest installer '
                       'with DolphinTool included, or select an already extracted game folder.')


def game_copy(path):
    source = Path(path).expanduser().resolve()
    if source.is_dir():
        return game_partition(source), False
    if source.is_file() and source.suffix.lower() in DISC_EXTENSIONS:
        return source, True
    raise InstallError('Choose your Super Mario Galaxy ISO, RVZ or WBFS file, '
                       'or an already extracted game folder.')


def dolphin_command(tool, *args, cwd=None):
    # Collected DolphinTool libraries need the frozen application's search path.
    # An external/source-mode tool uses the original system environment.
    bundled = getattr(sys, 'frozen', False) and Path(sys._MEIPASS) in Path(tool).resolve().parents
    env = os.environ.copy() if bundled else push_data.adb_environment()
    try:
        result = subprocess.run([tool] + list(args), cwd=cwd, env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors='replace',
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except OSError as error:
        raise InstallError('Could not start the disc extractor: ' + str(error)) from error
    if result.returncode:
        raise InstallError('Dolphin could not extract this game copy. Check that the file is complete '
                           'and readable.\n' + result.stdout.strip()[-4000:])
    return result.stdout


def extract_disc(image, destination):
    destination = Path(destination)
    if destination.exists():
        raise InstallError('Disc extraction requires a new temporary folder.')
    tool = find_dolphin_tool()
    print('Extracting %s with %s' % (image, tool), flush=True)
    output = dolphin_command(tool, 'extract', '-i', str(image), '-o', str(destination),
                             '--gameonly', '--quiet', cwd=str(destination.parent))
    if output.strip():
        print(output.strip(), flush=True)
    # Dolphin may return success after an incomplete extraction; validate the
    # game partition before handing it to the converter.
    return game_partition(destination)


@contextlib.contextmanager
def temporary_workspace(source, movies):
    """Choose a writable disk with enough space, then clean only our own folder."""
    source = Path(source).resolve()
    required = (16 if movies else 8) * GIB if source.is_file() else (10 if movies else 4) * GIB
    candidates = (cache_dir(), source.parent, Path(tempfile.gettempdir()))
    attempted = set()
    scratch = None
    for candidate in candidates:
        base = candidate.expanduser().resolve()
        if base in attempted or (source.is_dir() and (base == source or source in base.parents)):
            continue
        attempted.add(base)
        try:
            base.mkdir(parents=True, exist_ok=True)
            if shutil.disk_usage(base).free < required:
                continue
            scratch = tempfile.TemporaryDirectory(prefix='.galaxyquest-install-', dir=base)
            break
        except OSError as error:
            print('Temporary storage unavailable at %s: %s' % (base, error), flush=True)
    if scratch is None:
        raise InstallError('The computer needs at least %d GB free for this installation. '
                           'Free space on the system disk or the disk holding your game and try again.' % (required // GIB))
    with scratch as folder:
        print('Temporary working folder: ' + folder, flush=True)
        yield Path(folder)


def run_adb(adb, *args, serial=None):
    command = [adb] + (['-s', serial] if serial else []) + list(args)
    # Redirect all streams: windowed frozen apps have no standard handles.
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace',
                            env=push_data.adb_environment(),
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode:
        raise InstallError(result.stdout.strip() or 'The headset connection command failed.')
    return result.stdout.strip()


def list_devices(adb):
    devices = []
    for line in run_adb(adb, 'devices', '-l').splitlines():
        words = line.split()
        if len(words) >= 2 and not line.startswith(('List ', '*')):
            devices.append((words[0], words[1], ' '.join(words[2:])))
    return devices


def choose_device(devices, selected=''):
    ready = [serial for serial, state, _ in devices if state == 'device']
    if selected and selected in ready:
        return selected
    if selected:
        raise InstallError('The selected headset is no longer connected. Reconnect it and click Refresh.')
    if len(ready) == 1:
        return ready[0]
    if len(ready) > 1:
        raise InstallError('Several devices are connected. Select your headset in the list.')
    if any(state == 'unauthorized' for _, state, _ in devices):
        raise InstallError('Put on the headset and accept Allow USB debugging, '
                           'then click Refresh. You can select Always allow from this computer.')
    if any(state == 'offline' for _, state, _ in devices):
        raise InstallError('The headset is offline. Reconnect the USB cable, wake the headset and click Refresh.')
    raise InstallError('No headset found. Enable developer mode, connect a USB data cable, '
                       'and accept USB debugging in the headset. On Linux, also check your USB device permissions.')


def game_partition(folder):
    root = Path(folder).expanduser().resolve()
    for candidate in (root, root / 'DATA'):
        if ((candidate / 'sys' / 'fst.bin').is_file()
                and (candidate / 'sys' / 'main.dol').is_file()
                and (candidate / 'files' / 'ObjectData').is_dir()
                and (candidate / 'files' / 'StageData').is_dir()):
            return candidate
    raise InstallError('Select the whole extracted disc folder, or its DATA folder. '
                       'It must contain sys/main.dol, sys/fst.bin and the game files. '
                       'In Dolphin, use Extract Entire Disc.')


def free_device_bytes(adb, serial):
    output = run_adb(adb, 'shell', 'df -k /sdcard', serial=serial)
    for line in reversed(output.splitlines()):
        fields = line.split()
        if len(fields) >= 4 and fields[1].isdigit() and fields[3].isdigit():
            return int(fields[3]) * 1024
    raise InstallError('Could not check the headset storage. Reconnect it and try again.\n' + output)


def install_game(game, selected, movies, status):
    """All heavy work runs off the UI thread; errors stop before copying bad data."""
    source, is_image = game_copy(game)
    if is_image:
        find_dolphin_tool()  # Fail early, before doing any heavy work.
    adb = find_adb()
    apk = find_apk()
    status('Checking headset and storage...', 2)
    serial = choose_device(list_devices(adb), selected)
    # Prevent accidentally installing to an Android phone listed by adb.
    manufacturer = run_adb(adb, 'shell', 'getprop ro.product.manufacturer', serial=serial).lower()
    model = run_adb(adb, 'shell', 'getprop ro.product.model', serial=serial)
    if manufacturer not in ('oculus', 'meta'):
        raise InstallError('The selected device is %s (%s), not a Meta headset. '
                           'Disconnect other Android devices and select your headset.' % (model, manufacturer))
    print('Headset: %s (%s)' % (model, serial), flush=True)
    if free_device_bytes(adb, serial) < (7 if movies else 3) * GIB:
        raise InstallError('Free at least %d GB on the headset and try again.' % (7 if movies else 3))
    with temporary_workspace(source, movies) as scratch:
        cooked = scratch / 'cooked'
        if is_image:
            status('Extracting your disc with Dolphin...', 5)
            source = extract_disc(source, scratch / 'extracted')
        status('Converting your game...', 20)

        def progress(done, total):
            status('Converting game files: %d / %d' % (done, total),
                   20 + 45 * done / max(total, 1))

        result = cook.convert(str(source), str(cooked), with_movies=movies,
                              jobs=min(os.cpu_count() or 1, 8), progress=progress)
        if result:
            raise InstallError('Some game files could not be converted. '
                               'See the log for the filenames; extract the whole disc again and retry.')
        size = sum(path.stat().st_size for path in cooked.rglob('*') if path.is_file())
        if free_device_bytes(adb, serial) < size * 2 + apk.stat().st_size + 512 * 1024 ** 2:
            raise InstallError('The headset needs more free storage to copy and unpack this game '
                               '(about %.1f GB). Free space and retry.' % ((size * 2 + 512 * 1024 ** 2) / GIB))
        if shutil.disk_usage(scratch).free < size + 512 * 1024 ** 2:
            raise InstallError('The computer needs another %.1f GB to pack the converted game.' % (size / GIB))
        status('Installing GalaxyQuest...', 67)
        print(run_adb(adb, 'install', '--no-incremental', '-r', str(apk), serial=serial), flush=True)

        def copying(message):
            if message.startswith('packing'):
                status('Preparing game files for copying...', 72)
            elif message.startswith('copying'):
                status('Copying the game to your headset...', 80)
            else:
                status('Finishing installation on your headset...', 95)

        push_data.copy_data(str(cooked), adb=adb, serial=serial,
                            progress=copying, temp_dir=str(scratch))
    status('Installed! Open GalaxyQuest in Library > Unknown Sources on your headset.', 100)


def self_test():
    """Exercise imports and frozen multiprocessing without a headset or game."""
    import tkinter
    tkinter.Tcl()  # Verify bundled Tcl without opening a window/display.
    with tempfile.TemporaryDirectory() as scratch:
        src = Path(scratch) / 'input.txt'
        dst = Path(scratch) / 'output.txt'
        src.write_text('GalaxyQuest installer worker test', encoding='utf-8')
        with concurrent.futures.ProcessPoolExecutor(max_workers=2,
                mp_context=multiprocessing.get_context('spawn')) as pool:
            stats, notes, error = pool.submit(cook.process, (str(src), str(dst), 'copy')).result(timeout=60)
        assert error is None and stats['copied'] == 1 and dst.read_bytes() == src.read_bytes()
    if getattr(sys, 'frozen', False):
        assert find_apk().is_file()
        assert Path(find_adb()).is_file()
        assert '--gameonly' in dolphin_command(find_dolphin_tool(), 'extract', '--help')


class Installer:
    def __init__(self, root):
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
        self.tk, self.ttk = tk, ttk
        self.filedialog, self.messagebox = filedialog, messagebox
        self.root, self.events = root, queue.Queue()
        self.busy = False
        root.title('GalaxyQuest installer')
        root.geometry('760x560')
        root.minsize(650, 510)
        root.protocol('WM_DELETE_WINDOW', self.close)
        panel = ttk.Frame(root, padding=24)
        panel.pack(fill='both', expand=True)
        panel.columnconfigure(0, weight=1)
        panel.rowconfigure(12, weight=1)
        ttk.Label(panel, text='Install GalaxyQuest', font=('', 22, 'bold')).grid(row=0, column=0, sticky='w')
        ttk.Label(panel, text='Choose your game copy, connect your headset, then click Install.',
                  wraplength=670).grid(row=1, column=0, columnspan=2, sticky='w', pady=(6, 16))
        ttk.Label(panel, text='Your Super Mario Galaxy copy (ISO, RVZ or WBFS)').grid(row=2, column=0, sticky='w')
        self.folder = tk.StringVar()
        self.folder_entry = ttk.Entry(panel, textvariable=self.folder)
        self.folder_entry.grid(row=3, column=0, sticky='ew', pady=(4, 12))
        self.browse = ttk.Button(panel, text='Browse...', command=self.pick_game)
        self.browse.grid(row=3, column=1, padx=(8, 0), pady=(4, 12))
        self.movies = tk.BooleanVar(value=True)
        self.movies_check = ttk.Checkbutton(panel, text='Include opening and ending movies (recommended)', variable=self.movies)
        self.folder_button = ttk.Button(panel, text='Use an already extracted folder...', command=self.pick_folder)
        self.folder_button.grid(row=4, column=0, columnspan=2, sticky='w', pady=(0, 12))
        self.movies_check.grid(row=5, column=0, columnspan=2, sticky='w')
        ttk.Label(panel, text='Headset (developer mode and USB debugging must be enabled)').grid(row=6, column=0, sticky='w', pady=(16, 4))
        self.device = tk.StringVar()
        self.devices = ttk.Combobox(panel, textvariable=self.device, state='readonly')
        self.devices.grid(row=7, column=0, sticky='ew')
        self.refresh_button = ttk.Button(panel, text='Refresh', command=self.refresh)
        self.refresh_button.grid(row=7, column=1, padx=(8, 0))
        ttk.Button(panel, text='Developer mode help', command=lambda: webbrowser.open(DEVELOPER_HELP)).grid(row=8, column=0, sticky='w', pady=(4, 12))
        self.install_button = ttk.Button(panel, text='Install', command=self.start)
        self.install_button.grid(row=9, column=0, sticky='w', pady=(0, 8))
        self.status = tk.StringVar(value='Ready. Your game will be converted and copied to the headset.')
        ttk.Label(panel, textvariable=self.status, wraplength=670).grid(row=10, column=0, columnspan=2, sticky='w')
        self.bar = ttk.Progressbar(panel, maximum=100)
        self.bar.grid(row=11, column=0, columnspan=2, sticky='ew', pady=(8, 10))
        self.log = tk.Text(panel, height=6, state='disabled', wrap='word')
        self.log.grid(row=12, column=0, columnspan=2, sticky='nsew')
        self.root.after(100, self.drain)
        self.root.after(200, self.refresh)

    def pick_game(self):
        path = self.filedialog.askopenfilename(title='Choose your Super Mario Galaxy copy',
            filetypes=[('Game disc images', '*.iso *.rvz *.wbfs *.gcz *.wia *.ciso'), ('All files', '*')])
        if path:
            self.folder.set(path)

    def pick_folder(self):
        path = self.filedialog.askdirectory(title='Choose the extracted game folder', mustexist=True)
        if path:
            self.folder.set(path)

    def controls(self, enabled):
        state = 'normal' if enabled else 'disabled'
        for widget in (self.folder_entry, self.browse, self.movies_check, self.refresh_button,
                       self.folder_button, self.install_button):
            widget.configure(state=state)
        self.devices.configure(state='readonly' if enabled else 'disabled')

    def refresh(self):
        self.refresh_button.configure(state='disabled')

        def detect():
            try:
                devices = list_devices(find_adb())
                self.events.put(('devices', devices))
            except Exception as error:
                self.events.put(('detect-error', str(error)))
        threading.Thread(target=detect, daemon=True).start()

    def start(self):
        if not self.folder.get().strip():
            self.messagebox.showinfo('Choose your game', 'Click Browse and select your game copy.')
            return
        args = (self.folder.get(), self.device.get(), self.movies.get())
        self.busy = True
        self.controls(False)
        self.bar.configure(mode='determinate', value=0)
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.configure(state='disabled')
        threading.Thread(target=self.worker, args=args, daemon=True).start()

    def worker(self, folder, selected, movies):
        log_path = None
        try:
            # Logs survive cleanup and can be attached to bug reports.
            logs = cache_dir() / 'logs'
            logs.mkdir(parents=True, exist_ok=True)
            stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
            log_path = logs / ('install-%s.log' % stamp)
            with log_path.open('w', encoding='utf-8', buffering=1) as stream:
                with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
                    print('GalaxyQuest installer\nPython: %s\nGame: %s\nMovies: %s' %
                          (sys.version, folder, movies), flush=True)
                    try:
                        install_game(folder, selected, movies,
                                     lambda text, percent: self.events.put(('progress', text, percent)))
                    except BaseException:
                        traceback.print_exc()
                        raise
            self.events.put(('success', str(log_path)))
        except BaseException as error:
            detail = str(error)
            if isinstance(error, subprocess.CalledProcessError):
                detail = error.output or detail
            self.events.put(('error', detail, str(log_path) if log_path else 'Could not create a log.'))

    def drain(self):
        try:
            while True:
                event = self.events.get_nowait()
                kind = event[0]
                if kind == 'progress':
                    self.status.set(event[1])
                    self.bar.stop()
                    self.bar.configure(mode='determinate', value=event[2])
                    if event[1].startswith(('Extracting', 'Preparing', 'Copying', 'Finishing', 'Installing')):
                        self.bar.configure(mode='indeterminate')
                        self.bar.start(20)
                elif kind == 'devices':
                    if self.busy:
                        continue
                    ready = [serial for serial, state, _ in event[1] if state == 'device']
                    self.devices.configure(values=ready)
                    if self.device.get() not in ready:
                        self.device.set(ready[0] if len(ready) == 1 else '')
                    try:
                        choose_device(event[1], self.device.get())
                        self.status.set('Headset connected. Choose your game copy and click Install.')
                    except InstallError as error:
                        self.status.set(str(error))
                    if not self.busy:
                        self.refresh_button.configure(state='normal')
                elif kind == 'detect-error':
                    if self.busy:
                        continue
                    self.status.set(event[1])
                    if not self.busy:
                        self.refresh_button.configure(state='normal')
                elif kind in ('success', 'error'):
                    self.busy = False
                    self.controls(True)
                    self.bar.stop()
                    self.bar.configure(mode='determinate', value=100 if kind == 'success' else 0)
                    log_path = event[1] if kind == 'success' else event[2]
                    self.log.configure(state='normal')
                    if Path(log_path).is_file():
                        self.log.insert('end', Path(log_path).read_text(encoding='utf-8')[-16000:])
                    self.log.insert('end', '\nLog: ' + log_path)
                    self.log.see('end')
                    self.log.configure(state='disabled')
                    if kind == 'error':
                        self.status.set('Installation stopped. Fix the issue below and click Install to retry.')
                        self.messagebox.showerror('Installation stopped', event[1] + '\n\nLog: ' + log_path)
                    else:
                        self.messagebox.showinfo('GalaxyQuest installed',
                            'Open Library > Unknown Sources > GalaxyQuest on your headset.\n\nLog: ' + log_path)
        except queue.Empty:
            pass
        self.root.after(100, self.drain)

    def close(self):
        if self.busy:
            self.messagebox.showinfo('Installation running', 'Please wait for installation to finish before closing this window.')
            return
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--self-test', action='store_true', help='test the packaged converter and worker processes')
    ap.add_argument('--self-test-report', type=Path, help='write the self-test result to a file (for windowed builds)')
    ap.add_argument('--convert-only', nargs=2, metavar=('GAME', 'OUTPUT'), help='extract and convert without connecting a headset')
    ap.add_argument('--without-movies', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        try:
            self_test()
            report, result = 'PASS: converter imports, Tcl, worker processes and bundled assets\n', 0
        except BaseException:
            report, result = traceback.format_exc(), 1
        if args.self_test_report:
            args.self_test_report.write_text(report, encoding='utf-8')
        elif sys.stdout is not None:
            print(report)
        return result
    if args.convert_only:
        # No console in Windows builds: write conversion details beside output.
        output = Path(args.convert_only[1]).resolve()
        if output.exists():
            ap.error('--convert-only requires a new output folder so existing files are preserved')
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.with_name(output.name + '-conversion.log').open('w', encoding='utf-8', buffering=1) as log:
            with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                source, is_image = game_copy(args.convert_only[0])
                with temporary_workspace(source, not args.without_movies) as scratch:
                    if is_image:
                        source = extract_disc(source, scratch / 'extracted')
                    return cook.convert(str(source), str(output),
                                        jobs=min(os.cpu_count() or 1, 8), with_movies=not args.without_movies)
    import tkinter as tk
    root = tk.Tk()
    Installer(root)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
