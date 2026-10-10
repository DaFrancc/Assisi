"""release: package a build and keep what players get under releases/."""

from __future__ import annotations

from assisi import releases
from assisi.commands import package
from assisi.options import FRESH, PREVIOUS, Kind, Option, build_options, previous_options
from assisi.request import parse

NAME = "release"
HELP = "Package a build and keep the game and its pak in releases/<version>/, for later packages to match"
OPTIONS = build_options(sanitizers=False, required=True) + (
    Option("version", Kind.STRING, "what to call this release, e.g. 1.2", required=True),
) + previous_options()
RECORDED = True


def validate(request, project) -> None:
    package.validate(request, project)
    releases.check_version(project, request.get("version"))


def package_request(request, project):
    """The package this release is: the same build, laid out the same way."""
    argv = ["package", *request.specs(project)[0].argv()]
    if request.get(PREVIOUS) is not None:
        argv += ["--previous", request.get(PREVIOUS)]
    if request.get(FRESH):
        argv.append("--fresh")
    return parse(argv, project)


def execute(request, context) -> int:
    project = context.project
    packaged = package_request(request, project)
    # Through package, so a Steam Runtime release is packaged in its container
    # like any other, and kept here on the host, where releases/ lives.
    package.execute(packaged, context)
    spec = request.specs(project)[0]
    kept = releases.keep(project, request.get("version"), spec, request.trees(project)[0], request.to_argv())
    print(f"released {request.get('version')}: {kept.relative_to(project.root)}/")
    return 0
