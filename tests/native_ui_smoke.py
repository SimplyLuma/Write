#!/usr/bin/env python3
"""Drive the production Write binary through GTK's AT-SPI surface."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import zipfile
from xml.etree import ElementTree

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi


CANVAS_NAME = "Document editing canvas"
DETERMINISTIC_TEXT = "Write native GTK smoke round trip 2026-08-23"
DEFAULT_TIMEOUT = 30.0


class SmokeFailure(RuntimeError):
    pass


def node_name(node: Atspi.Accessible) -> str:
    try:
        return node.get_name() or ""
    except Exception:
        return ""


def node_role(node: Atspi.Accessible) -> Atspi.Role:
    try:
        return node.get_role()
    except Exception:
        return Atspi.Role.INVALID


def nodes_below(root: Atspi.Accessible):
    pending = [root]
    while pending:
        node = pending.pop()
        yield node
        try:
            children = [
                node.get_child_at_index(index)
                for index in range(node.get_child_count())
            ]
        except Exception:
            continue
        pending.extend(reversed([child for child in children if child is not None]))


def showing(node: Atspi.Accessible) -> bool:
    try:
        states = node.get_state_set()
        return states.contains(Atspi.StateType.SHOWING) and states.contains(
            Atspi.StateType.VISIBLE
        )
    except Exception:
        return False


def enabled(node: Atspi.Accessible) -> bool:
    try:
        states = node.get_state_set()
        return states.contains(Atspi.StateType.ENABLED) or states.contains(
            Atspi.StateType.SENSITIVE
        )
    except Exception:
        return False


def wait_until(predicate, description: str, timeout: float = DEFAULT_TIMEOUT):
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except Exception as error:
            last_error = error
        time.sleep(0.1)
    detail = f" ({last_error})" if last_error is not None else ""
    raise SmokeFailure(f"Timed out waiting for {description}{detail}")


def wait_for_node(
    root: Atspi.Accessible,
    predicate,
    description: str,
    timeout: float = DEFAULT_TIMEOUT,
) -> Atspi.Accessible:
    return wait_until(
        lambda: next(
            (node for node in nodes_below(root) if predicate(node)),
            None,
        ),
        description,
        timeout,
    )


def wait_for_application(
    process: subprocess.Popen, log_path: Path
) -> Atspi.Accessible:
    desktop = Atspi.get_desktop(0)

    def application():
        return_code = process.poll()
        if return_code is not None:
            tail = log_path.read_text(errors="replace")[-4000:]
            raise SmokeFailure(
                f"Write exited with status {return_code} before publishing "
                f"its AT-SPI application; log tail:\n{tail}"
            )
        for index in range(desktop.get_child_count()):
            candidate = desktop.get_child_at_index(index)
            try:
                if candidate.get_process_id() == process.pid:
                    return candidate
            except Exception:
                continue
        return None

    return wait_until(application, f"AT-SPI application for process {process.pid}")


def dump_accessibility_tree(path: Path) -> None:
    lines = []

    def visit(node: Atspi.Accessible, depth: int) -> None:
        try:
            role = node.get_role_name() or "unknown"
            name = node_name(node)
            actions = []
            if node.is_action():
                action = node.get_action_iface()
                actions = [
                    action.get_action_name(index) or ""
                    for index in range(action.get_n_actions())
                ]
            lines.append(
                f"{'  ' * depth}{role} name={name!r} "
                f"showing={showing(node)} enabled={enabled(node)} actions={actions!r}"
            )
            for index in range(node.get_child_count()):
                child = node.get_child_at_index(index)
                if child is not None:
                    visit(child, depth + 1)
        except Exception as error:
            lines.append(f"{'  ' * depth}<unavailable: {error}>")

    visit(Atspi.get_desktop(0), 0)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def invoke(node: Atspi.Accessible, description: str) -> None:
    if not enabled(node):
        raise SmokeFailure(f"{description} is present but disabled")
    try:
        action = node.get_action_iface()
        action_names = [
            (action.get_action_name(index) or "").lower()
            for index in range(action.get_n_actions())
        ]
        for preferred in ("click", "activate", "press", "open"):
            if preferred in action_names:
                if action.do_action(action_names.index(preferred)):
                    return
                break
        if action_names and action.do_action(0):
            return
    except Exception as error:
        raise SmokeFailure(f"Could not invoke {description}: {error}") from error
    raise SmokeFailure(f"Could not invoke {description}: no usable AT-SPI action")


def click_named(
    root: Atspi.Accessible,
    name: str,
    description: str,
    *,
    exact: bool = True,
) -> Atspi.Accessible:
    def matches(node: Atspi.Accessible) -> bool:
        candidate = node_name(node)
        name_matches = candidate == name if exact else name in candidate
        if not name_matches or not showing(node) or not enabled(node):
            return False
        try:
            return node.is_action() and node.get_action_iface().get_n_actions() > 0
        except Exception:
            return False

    node = wait_for_node(root, matches, description)
    invoke(node, description)
    return node


def accessible_text(node: Atspi.Accessible) -> str:
    try:
        if not node.is_text():
            return ""
        return node.get_text_iface().get_text(0, -1) or ""
    except Exception:
        return ""


def screen_extents(node: Atspi.Accessible, description: str):
    try:
        extents = node.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
    except Exception as error:
        raise SmokeFailure(f"Could not locate {description}: {error}") from error
    if extents.width < 2 or extents.height < 2:
        raise SmokeFailure(
            f"{description} has invalid bounds "
            f"({extents.x}, {extents.y}, {extents.width}, {extents.height})"
        )
    return extents


def set_save_name(chooser: Atspi.Accessible, filename: str) -> None:
    def filename_entry():
        candidates = []
        for node in nodes_below(chooser):
            try:
                if node.is_editable_text() and showing(node):
                    candidates.append(node)
            except Exception:
                continue
        for candidate in candidates:
            entry_text = accessible_text(candidate)
            if filename in entry_text or entry_text.endswith(".docx"):
                return candidate
        for candidate in candidates:
            if "name" in node_name(candidate).casefold():
                return candidate
        return candidates[0] if len(candidates) == 1 else None

    entry = wait_until(filename_entry, "the Save As filename entry")
    try:
        if not entry.get_editable_text_iface().set_text_contents(filename):
            raise SmokeFailure("the AT-SPI EditableText operation returned false")
    except Exception as error:
        raise SmokeFailure(f"Could not set the Save As filename: {error}") from error


def desktop_file_chooser(process_id: int) -> Atspi.Accessible | None:
    desktop = Atspi.get_desktop(0)
    for node in nodes_below(desktop):
        try:
            if (
                node.get_process_id() == process_id
                and (
                    node_role(node) == Atspi.Role.FILE_CHOOSER
                    or (
                        node_role(node) in (Atspi.Role.DIALOG, Atspi.Role.FRAME)
                        and "save" in node_name(node).casefold()
                    )
                )
                and showing(node)
            ):
                return node
        except Exception:
            continue
    return None


def quit_through_ui(application: Atspi.Accessible) -> None:
    # AppKit 1.luma.19 retains the stable generic identity label. Accept the
    # previous product-title name only while mixed-package developer images are
    # still useful for source-admission diagnostics.
    identity_names = {"Application menu", "Write"}

    def identity_menu(node: Atspi.Accessible) -> bool:
        if node_name(node) not in identity_names or not showing(node) or not enabled(node):
            return False
        try:
            return node.is_action() and node.get_action_iface().get_n_actions() > 0
        except Exception:
            return False

    identity = wait_for_node(application, identity_menu, "the AppKit identity menu")
    invoke(identity, "the AppKit identity menu")
    # AppKit 1.luma.19's native GMenu popover does not publish its items in the
    # headless Xvfb/AT-SPI lane. Opening the AppKit-owned identity control still
    # verifies that the menu surface is actionable; close it and exercise the
    # same registered Quit command through its standard keyboard route. Keep
    # the fallback explicit so the platform accessibility gap is not mistaken
    # for application-owned menu chrome.
    time.sleep(0.25)
    subprocess.run(
        ["xdotool", "key", "Escape", "ctrl+q"],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def wait_for_clean_exit(process: subprocess.Popen, log_path: Path) -> None:
    try:
        return_code = process.wait(timeout=15)
    except subprocess.TimeoutExpired as error:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        raise SmokeFailure("Write did not exit after invoking Quit") from error
    if return_code != 0:
        tail = log_path.read_text(errors="replace")[-4000:]
        raise SmokeFailure(
            f"Write exited with status {return_code}; log tail:\n{tail}"
        )


def verify_docx_text(path: Path) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise SmokeFailure(f"Save As did not create a nonempty DOCX at {path}")
    try:
        with zipfile.ZipFile(path) as archive:
            document_xml = archive.read("word/document.xml")
        visible_text = "".join(ElementTree.fromstring(document_xml).itertext())
    except (OSError, KeyError, zipfile.BadZipFile, ElementTree.ParseError) as error:
        raise SmokeFailure(f"Saved DOCX could not be inspected: {error}") from error
    if DETERMINISTIC_TEXT not in visible_text:
        raise SmokeFailure(
            "Saved DOCX did not contain the deterministic text entered through GTK"
        )


def assert_no_fatal_diagnostics(*logs: Path) -> None:
    forbidden = (
        "Gtk-WARNING",
        "Gtk-CRITICAL",
        "GLib-GObject-WARNING",
        "GLib-GObject-CRITICAL",
        "GLib-ERROR",
    )
    for log in logs:
        contents = log.read_text(errors="replace")
        for marker in forbidden:
            if marker in contents:
                raise SmokeFailure(f"{marker} was emitted; see {log}")


def isolated_environment(artifacts: Path) -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "G_DEBUG": "fatal-warnings",
            "GDK_BACKEND": environment.get("GDK_BACKEND", "wayland"),
            "GSETTINGS_BACKEND": "keyfile",
            "GTK_A11Y": "atspi",
            "LUMA_LIBREOFFICE_PROGRAM": environment.get(
                "LUMA_LIBREOFFICE_PROGRAM", "/usr/lib64/libreoffice/program"
            ),
            "SAL_USE_VCLPLUGIN": "svp",
            "XDG_CACHE_HOME": str(artifacts / "cache"),
            "XDG_CONFIG_HOME": str(artifacts / "config"),
            "XDG_DATA_HOME": str(artifacts / "data"),
            "XDG_STATE_HOME": str(artifacts / "state"),
        }
    )
    environment.pop("GTK_USE_PORTAL", None)
    for key in ("XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"):
        Path(environment[key]).mkdir(parents=True, exist_ok=True)
    # The save sheet offers Documents first for an untitled document; make
    # that this run's artifacts folder.
    (Path(environment["XDG_CONFIG_HOME"]) / "user-dirs.dirs").write_text(
        f'XDG_DOCUMENTS_DIR="{artifacts.resolve()}"\n', encoding="utf-8"
    )
    return environment


def configure_chooser(environment: dict[str, str], artifacts: Path) -> None:
    settings = "org.gtk.Settings.FileChooser"
    for key, value in (
        ("startup-mode", "cwd"),
        ("location-mode", "filename-entry"),
        ("last-folder-uri", artifacts.resolve().as_uri()),
    ):
        subprocess.run(
            ["gsettings", "set", settings, key, value],
            cwd=artifacts,
            env=environment,
            check=True,
            stdout=subprocess.DEVNULL,
        )


def launch(binary: Path, arguments: list[str], environment: dict[str, str], cwd: Path, log: Path):
    stream = log.open("wb")
    process = subprocess.Popen(
        [str(binary), *arguments],
        cwd=cwd,
        env=environment,
        stdout=stream,
        stderr=subprocess.STDOUT,
    )
    return process, stream


def capture_editor(path: Path, environment: dict[str, str]) -> None:
    # A separate GDK3 process captures Xvfb without importing another GTK
    # major version into the AT-SPI driver or the production application.
    subprocess.run([
        "python3", "-c",
        "import gi,sys; gi.require_version('Gdk','3.0'); "
        "from gi.repository import Gdk; w=Gdk.get_default_root_window(); "
        "p=Gdk.pixbuf_get_from_window(w,0,0,w.get_width(),w.get_height()); "
        "p.savev(sys.argv[1],'png',[],[])", str(path)
    ], env=environment, check=True)


def create_edit_save(
    binary: Path, output: Path, environment: dict[str, str], log: Path
) -> None:
    process, stream = launch(binary, [], environment, output.parent, log)
    try:
        application = wait_for_application(process, log)
        click_named(
            application,
            "New document",
            "the New document action",
        )
        canvas = wait_for_node(
            application,
            lambda node: showing(node)
            and (
                node_name(node) == CANVAS_NAME
                or node_role(node) == Atspi.Role.SCROLL_PANE
            ),
            "the real document editing canvas",
        )
        window_ids = subprocess.run(
            [
                "xdotool",
                "search",
                "--sync",
                "--onlyvisible",
                "--pid",
                str(process.pid),
            ],
            env=environment,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        if not window_ids:
            raise SmokeFailure("xdotool could not find the Write window")
        geometry = subprocess.run(
            ["xdotool", "getwindowgeometry", "--shell", window_ids[-1]],
            env=environment,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        (log.parent / "x11-window.txt").write_text(
            f"windows={window_ids!r}\nselected={window_ids[-1]}\n{geometry}",
            encoding="utf-8",
        )
        subprocess.run(
            ["xdotool", "windowfocus", "--sync", window_ids[-1]],
            env=environment,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        canvas_extents = screen_extents(canvas, "the real document editing canvas")
        canvas_x = canvas_extents.x + canvas_extents.width // 2
        canvas_y = canvas_extents.y + canvas_extents.height // 2
        with (log.parent / "x11-window.txt").open("a", encoding="utf-8") as stream:
            stream.write(
                "canvas="
                f"{canvas_extents.x},{canvas_extents.y},"
                f"{canvas_extents.width},{canvas_extents.height}\n"
                f"canvas_click={canvas_x},{canvas_y}\n"
            )
        subprocess.run(
            [
                "xdotool",
                "mousemove",
                "--sync",
                str(canvas_x),
                str(canvas_y),
                "click",
                "1",
            ],
            env=environment,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        # GTK hands focus from the scroll viewport to LibreOfficeKit's canvas
        # asynchronously. Without this settlement interval, fast builders can
        # send the leading keystrokes before the engine owns keyboard focus.
        time.sleep(2.0)
        subprocess.run(
            [
                "xdotool",
                "type",
                "--clearmodifiers",
                "--delay",
                "25",
                DETERMINISTIC_TEXT,
            ],
            env=environment,
            check=True,
            stdout=subprocess.DEVNULL,
        )

        def modified_title_visible() -> bool:
            return_code = process.poll()
            if return_code is not None:
                tail = log.read_text(errors="replace")[-4000:]
                raise SmokeFailure(
                    f"Write exited with status {return_code} after editing; "
                    f"log tail:\n{tail}"
                )
            current_windows = subprocess.run(
                [
                    "xdotool",
                    "search",
                    "--onlyvisible",
                    "--pid",
                    str(process.pid),
                ],
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            ).stdout.splitlines()
            for current_window in reversed(current_windows):
                title_result = subprocess.run(
                    ["xdotool", "getwindowname", current_window],
                    env=environment,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if title_result.returncode == 0 and "•" in title_result.stdout:
                    return True
            return False

        wait_until(
            modified_title_visible,
            "the modified-document marker in the window title",
            timeout=45,
        )
        capture_editor(log.parent / "edited-document.png", environment)
        # Reach the real unsaved condition. The platform's save sheet asks:
        # Escape keeps the document; the Save As chooser can still be
        # cancelled without writing; then the untitled document is named on
        # the sheet and Enter writes it into Documents with no second chooser.
        def decision_visible():
            return next((node for node in nodes_below(application)
                         if showing(node) and node_role(node) in (Atspi.Role.ALERT, Atspi.Role.DIALOG)
                         and "before" in node_name(node)), None)
        subprocess.run(["xdotool", "key", "ctrl+q"], check=True)
        decision = wait_until(decision_visible, "the platform's save sheet")
        if node_name(decision) != "Save “Untitled document” before closing?":
            raise SmokeFailure(f"Unexpected save sheet title: {node_name(decision)!r}")
        for name in ("Save", "Cancel"):
            wait_for_node(decision, lambda node, n=name: node_name(node) == n and showing(node), name)
        wait_for_node(decision, lambda node: node_name(node).startswith("Don’t save") and showing(node),
                      "Don’t save")
        wait_for_node(decision, lambda node: node_name(node) == "Name" and showing(node), "the Name field")
        subprocess.run(["xdotool", "key", "Escape"], check=True)
        wait_until(lambda: not decision_visible(), "Escape cancelling the close")
        if process.poll() is not None or not modified_title_visible() or output.exists():
            raise SmokeFailure("Cancelling the decision changed the unsaved document")
        # v71 keeps Save in the document menu and its standard accelerator.
        subprocess.run(["xdotool", "key", "ctrl+s"], env=environment, check=True)
        cancelled_chooser = wait_until(lambda: desktop_file_chooser(process.pid), "Save As opening the chooser")
        click_named(cancelled_chooser, "Cancel", "Cancel in Save As")
        wait_until(lambda: desktop_file_chooser(process.pid) is None, "Save As cancellation")
        if process.poll() is not None or not modified_title_visible() or output.exists():
            raise SmokeFailure("Cancelling Save As lost or saved the document")
        subprocess.run(["xdotool", "key", "ctrl+q"], check=True)
        wait_until(decision_visible, "the reopened save sheet")
        time.sleep(0.4)
        # The Name field opens focused with its text selected: typing replaces it.
        subprocess.run(["xdotool", "type", "--delay", "40", output.stem], check=True)
        subprocess.run(["xdotool", "key", "Return"], check=True)
        wait_until(
            lambda: output.is_file() and output.stat().st_size > 0,
            f"the DOCX the save sheet wrote at {output}",
            timeout=45,
        )
        wait_for_clean_exit(process, log)
        verify_docx_text(output)
    except Exception:
        try:
            dump_accessibility_tree(log.parent / "atspi-tree-live-failure.txt")
        except Exception:
            pass
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        raise
    finally:
        stream.close()


def reopen_and_verify(
    binary: Path, output: Path, environment: dict[str, str], log: Path
) -> None:
    process, stream = launch(binary, [str(output)], environment, output.parent, log)
    try:
        application = wait_for_application(process, log)
        wait_for_node(
            application,
            lambda node: node_name(node) == f"{output.name} — Write"
            and node_role(node) == Atspi.Role.FRAME and showing(node),
            "the reopened document title",
            timeout=45,
        )
        wait_for_node(
            application,
            lambda node: showing(node)
            and (
                node_name(node) == CANVAS_NAME
                or node_role(node) == Atspi.Role.SCROLL_PANE
            ),
            "the reopened production document canvas",
            timeout=45,
        )
        quit_through_ui(application)
        wait_for_clean_exit(process, log)
    except Exception:
        try:
            dump_accessibility_tree(log.parent / "atspi-tree-live-failure.txt")
        except Exception:
            pass
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        raise
    finally:
        stream.close()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    binary = arguments.binary.resolve()
    artifacts = arguments.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    output = artifacts / "native-ui-round-trip.docx"
    first_log = artifacts / "application-create-save.log"
    reopen_log = artifacts / "application-reopen.log"

    if output.exists():
        output.unlink()
    environment = isolated_environment(artifacts)
    configure_chooser(environment, artifacts)
    Atspi.init()
    try:
        try:
            create_edit_save(binary, output, environment, first_log)
            reopen_and_verify(binary, output, environment, reopen_log)
            assert_no_fatal_diagnostics(first_log, reopen_log)
        except Exception:
            try:
                dump_accessibility_tree(artifacts / "atspi-tree-on-failure.txt")
            except Exception as dump_error:
                print(
                    f"Could not capture the failure accessibility tree: {dump_error}",
                    file=sys.stderr,
                )
            print(f"Native UI smoke artifacts: {artifacts}", file=sys.stderr)
            raise
    finally:
        Atspi.exit()
    print(
        "native_ui_smoke: PASS "
        f"(typed, Save As DOCX, validated content, quit, reopened: {output})"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SmokeFailure as error:
        print(f"native_ui_smoke: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1) from error
