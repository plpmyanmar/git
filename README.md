`microsoft/git` and the Scalar CLI
==================================

[![CI/PR](https://github.com/microsoft/git/actions/workflows/main.yml/badge.svg)](https://github.com/microsoft/git/actions/workflows/main.yml)

This is `microsoft/git`, a special Git distribution to support monorepo scenarios. If you are _not_
working in a monorepo, you are likely searching for
[Git for Windows](http://git-for-windows.github.io/) instead of this codebase.

In addition to the Git command-line interface (CLI), `microsoft/git` includes the Scalar CLI to
further enable working with extremely large repositories. Scalar is a tool to apply the latest
recommendations and use the most advanced Git features. You can read
[the Scalar CLI documentation](contrib/scalar/scalar.txt) or read our
[Scalar user guide](contrib/scalar/docs/index.md) including
[the philosophy of Scalar](contrib/scalar/docs/philosophy.md).

If you encounter problems with `microsoft/git`, please report them as
[GitHub issues](https://github.com/microsoft/git/issues).

Why is this fork needed?
=========================================================

Git is awesome - it's a fast, scalable, distributed version control system with an unusually rich
command set that provides both high-level operations and full access to internals. What more could
you ask for?

Well, because Git is a distributed version control system, each Git repository has a copy of all
files in the entire history. As large repositories, aka _monorepos_ grow, Git can struggle to
manage all that data. As Git commands like `status` and `fetch` get slower, developers stop waiting
and start switching context. And context switches harm developer productivity.

`microsoft/git` is focused on addressing these performance woes and making the monorepo developer
experience first-class. The Scalar CLI packages all of these recommendations into a simple set of
commands.

One major feature that Scalar recommends is [partial clone](https://github.blog/2020-12-21-get-up-to-speed-with-partial-clone-and-shallow-clone/),
which reduces the amount of data transferred in order to work with a Git repository. While several
services such as GitHub support partial clone, Azure Repos instead has an older version of this
functionality called
[the GVFS protocol](https://docs.microsoft.com/en-us/azure/devops/learn/git/gvfs-architecture#gvfs-protocol).
The integration with the GVFS protocol present in `microsoft/git` is not appropriate to include in
the core Git client because partial clone is the official version of that functionality.

Downloading and Installing
=========================================================

If you're working in a monorepo and want to take advantage of the performance boosts in
`microsoft/git`, then you can download the latest version installer for your OS from the
[Releases page](https://github.com/microsoft/git/releases). Alternatively, you can opt to install
via the command line, using the below instructions for supported OSes:

## Windows

__Note:__ Winget is still in public preview, meaning you currently
[need to take special installation steps](https://docs.microsoft.com/en-us/windows/package-manager/winget/#install-winget):
Either manually install the `.appxbundle` available at the
[preview version of App Installer](https://www.microsoft.com/p/app-installer/9nblggh4nns1?ocid=9nblggh4nns1_ORSEARCH_Bing&rtc=1&activetab=pivot:overviewtab),
or participate in the
[Windows Insider flight ring](https://insider.windows.com/https://insider.windows.com/)
since `winget` is available by default on preview versions of Windows.

To install with Winget, run

```shell
winget install microsoft/git
```

Double-check that you have the right version by running these commands,
which should have the same output:

```shell
git version
scalar version
```

To upgrade `microsoft/git`, use the following Git command, which will download and install the latest
release.

```shell
git update-microsoft-git
```

You may also be alerted with a notification to upgrade, which presents a single-click process for
running `git update-microsoft-git`.

## macOS

To install `microsoft/git` on macOS, first [be sure that Homebrew is installed](https://brew.sh/) then
install the `microsoft-git` cask with these steps:

```shell
brew tap microsoft/git
brew install --cask microsoft-git
```

Double-check that you have the right version by running these commands,
which should have the same output:

```shell
git version
scalar version
```

To upgrade microsoft/git, you can run the necessary `brew` commands:

```shell
brew update
brew upgrade --cask microsoft-git
```

Or you can run the `git update-microsoft-git` command, which will run those brew commands for you.

## Linux

`apt-get` support is available for Ubuntu Bionic Beaver (18.04) and Hirsute
Hippo (21.04). Take the following steps to set up and install based on the
version you are running:

### Ubuntu 18.04 (Bionic)

```shell
curl -sSL https://packages.microsoft.com/keys/microsoft.asc | sudo apt-key add -
sudo apt-add-repository https://packages.microsoft.com/ubuntu/18.04/prod
sudo apt-get update
sudo apt-get install microsoft-git
```

### Ubuntu 21.04 (Hirsute)

```shell
curl -sSL https://packages.microsoft.com/config/ubuntu/21.04/prod.list | sudo tee /etc/apt/sources.list.d/microsoft-prod.list
curl -sSL https://packages.microsoft.com/keys/microsoft.asc | sudo tee /etc/apt/trusted.gpg.d/microsoft.asc
sudo apt-get update
sudo apt-get install microsoft-git
```

### Other Ubuntu/Debian distributions

Please use the most recent
[`.deb` package](https://github.com/microsoft/git/releases). For example,
you can download a specific version as follows:

```shell
wget -O microsoft-git.deb https://github.com/microsoft/git/releases/download/v2.33.0.vfs.0.0/microsoft-git_2.33.0.vfs.0.0.deb
sudo dpkg -i microsoft-git.deb
```

Double-check that you have the right version by running these commands,
which should have the same output:

```shell
git version
scalar version
```
### Non-Ubuntu/Debian distributions
You will need to compile and install `microsoft/git` from source:

```shell
git clone https://github.com/microsoft/git microsoft-git
cd microsoft-git
make -j12 prefix=/usr/local INCLUDE_SCALAR=YesPlease
sudo make -j12 prefix=/usr/local INCLUDE_SCALAR=YesPlease install
```

For more assistance building Git from source, see
[the INSTALL file in the core Git project](https://github.com/git/git/blob/master/INSTALL).

Contributing
=========================================================

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit <https://cla.microsoft.com.>

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
