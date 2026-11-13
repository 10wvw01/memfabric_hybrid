"""Exercise source checkout and update behavior using temporary Git repositories."""
import subprocess
from pathlib import Path

import pytest

HELPER = Path(__file__).resolve().parents[4] / "source_repos.sh"


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def commit(path, text):
    (path / "source.txt").write_text(text)
    git(path, "add", "source.txt")
    git(path, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "update")
    return git(path, "rev-parse", "HEAD")


@pytest.fixture
def repos(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    git(source, "init", "-b", "release")
    commit(source, "first\n")
    remote = tmp_path / "remote.git"
    subprocess.run(["git", "clone", "--bare", str(source), str(remote)], check=True, capture_output=True)
    return source, remote, tmp_path / "checkout"


def sync(remote, directory):
    return subprocess.run(
        ["bash", "-c", 'source "$1"; sync_source_repo "$2" "$3" release',
         "bash", str(HELPER), str(directory), str(remote)],
        capture_output=True, text=True,
    )


def test_clone_and_repeat_preserve_revision(repos):
    source, remote, directory = repos
    assert sync(remote, directory).returncode == 0
    expected = git(source, "rev-parse", "HEAD")
    assert git(directory, "branch", "--show-current") == "release"
    assert sync(remote, directory).returncode == 0
    assert git(directory, "rev-parse", "HEAD") == expected


def test_fast_forward_existing_checkout(repos):
    source, remote, directory = repos
    assert sync(remote, directory).returncode == 0
    expected = commit(source, "second\n")
    git(source, "push", str(remote), "release")
    assert sync(remote, directory).returncode == 0
    assert git(directory, "rev-parse", "HEAD") == expected


def test_existing_branch_commits_are_preserved(repos):
    _, remote, directory = repos
    assert sync(remote, directory).returncode == 0
    expected = commit(directory, "additional\n")
    assert sync(remote, directory).returncode == 0
    assert git(directory, "rev-parse", "HEAD") == expected


def test_uncommitted_sources_are_not_overwritten(repos):
    _, remote, directory = repos
    assert sync(remote, directory).returncode == 0
    (directory / "source.txt").write_text("uncommitted\n")
    result = sync(remote, directory)
    assert result.returncode != 0
    assert "uncommitted changes" in result.stderr
    assert (directory / "source.txt").read_text() == "uncommitted\n"


def test_switches_to_requested_branch(repos):
    _, remote, directory = repos
    assert sync(remote, directory).returncode == 0
    git(directory, "checkout", "-b", "other")
    assert sync(remote, directory).returncode == 0
    assert git(directory, "branch", "--show-current") == "release"


@pytest.fixture
def deployment(repos):
    source, _, _ = repos
    git(source, "branch", "-m", "trunk_for_escc_v1.2.1")
    fabric = source / "3rdparty/memfabric_hybrid"
    fabric.mkdir(parents=True)
    git(fabric, "init", "-b", "trunk_for_escc_v1.2.0")
    revision = commit(fabric, "fabric\n")
    git(source, "update-index", "--add", "--cacheinfo", f"160000,{revision},3rdparty/memfabric_hybrid")
    commit(source, "cache\n")
    library = fabric / "output/hybm/lib64/libmf_hybm_core.so"
    library.parent.mkdir(parents=True)
    library.write_bytes(b"first build")
    manifest = source.parent / "manifest.json"
    command = ["python3", str(HELPER.with_name("source_manifest.py")), "write",
               "--memcache-dir", str(source), "--output", str(manifest)]
    subprocess.run(command, check=True, capture_output=True)
    command[2] = "verify"
    return source, fabric, library, command


def test_verification_rejects_changed_source_revision(deployment):
    _, fabric, _, command = deployment
    commit(fabric, "new revision\n")
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 1
    assert "changed since setup" in result.stderr


def test_verification_rejects_changed_build_library(deployment):
    _, _, library, command = deployment
    library.write_bytes(b"different build")
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 1
    assert "changed since setup" in result.stderr


def test_diverged_branch_is_not_reset(repos):
    source, remote, directory = repos
    assert sync(remote, directory).returncode == 0
    expected = commit(directory, "local change\n")
    commit(source, "upstream change\n")
    git(source, "push", str(remote), "release")
    result = sync(remote, directory)
    assert result.returncode != 0
    assert "Cannot fast-forward" in result.stderr
    assert git(directory, "rev-parse", "HEAD") == expected
