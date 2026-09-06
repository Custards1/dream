# Edit this configuration file to define what 'should be installed on
# your system. Help is available in the configuration.nix(5) man page, on
# https://search.nixos.org/options and in the NixOS manual (`nixos-help`).

{ config, lib, pkgs, ... }:
let
nixvim = import (fetchGit {
		url = "https://github.com/nix-community/nixvim";
		});
desktop="kde"; # change me between cosmic and kde for easy switch
in
{
	imports =
		[
		<home-manager/nixos>
		# Include the results of the hardware scan.
		./hardware-configuration.nix
# ./neovim-custards.nix
			nixvim.nixosModules.nixvim
		];

# Use the systemd-boot EFI boot loader.
	boot.loader.systemd-boot.enable = true;
	boot.loader.efi.canTouchEfiVariables = true;
	boot.loader.grub.device="nodev";
# Use latest kernel.
	boot.kernelPackages = pkgs.linuxPackages;
	nix.settings.max-jobs=24;	
environment.sessionVariables.NIXOS_OZONE_WL = "1";

	networking.hostName = "custards"; # Define your hostname.

# Configure network connections interactively with nmcli or nmtui.
		networking.networkmanager.enable = true;

# Set your time zone.
	time.timeZone = "US/Mountain";

# Configure network proxy if necessary
# networking.proxy.default = "http://user:password@proxy:port/";
# networking.proxy.noProxy = "127.0.0.1,localhost,internal.domain";

# Select internationalisation properties.
	i18n.defaultLocale = "en_US.UTF-8";
	console = {
		font = "Lat2-Terminus16";
#  keyMap = "us";
		useXkbConfig = true; # use xkb.options in tty.
	};
	hardware.bluetooth.enable=true;
	hardware.bluetooth.powerOnBoot=true;
	nixpkgs.config.allowUnfree=true;
	hardware.nvidia.package = config.boot.kernelPackages.nvidiaPackages.production;
	hardware.graphics.enable=true;
	hardware.nvidia= {
		open=false;
		modesetting={enable=true;};
	};
	

	environment.sessionVariables.NIX_OZONE_WL = "1";

# Enable the X11 windowing system.
	services.xserver.videoDrivers  = [ "nvidia" ];
	services.xserver.enable = true;
systemd.services.bluetooth = {
  wantedBy = [ "multi-user.target" ];
  before = [ "display-manager.service" ];
};
#  programs.sway = {
#  	enable = true;
# wrapperFeatures.gtk = true;
#  };
#   services.xserver.desktopManager.sway.enable=true;


# Enable the COSMIC login manager
	services.displayManager.cosmic-greeter.enable = lib.mkIf (desktop == "cosmic") true;

# Enable the COSMIC desktop environment
	services.desktopManager.cosmic.enable = lib.mkIf (desktop == "cosmic") true;
	services.system76-scheduler.enable = lib.mkIf (desktop == "cosmic") true;#sightly improve COSMIC performance

	# services.displayManager.plasma-login-manager.enable=lib.mkIf (desktop == "kde") true;
	services.desktopManager.plasma6.enable = lib.mkIf (desktop == "kde") true;
	services.displayManager.sddm.enable = lib.mkIf (desktop == "kde") true;
	services.displayManager = {
	autoLogin.enable = true;
	autoLogin.user = "blake";
	};
environment.systemPackages = with pkgs; [
  # KDE Utilities
  kdePackages.discover # Optional: Software center for Flatpaks/firmware updates
  kdePackages.kcalc # Calculator
  kdePackages.kcharselect # Character map
  kdePackages.kclock # Clock app
  kdePackages.kcolorchooser # Color picker
  kdePackages.kolourpaint # Simple paint program
  kdePackages.ksystemlog # System log viewer
  kdePackages.sddm-kcm # SDDM configuration module
  kdiff3 # File/directory comparison tool
  
  # Hardware/System Utilities (Optional)
  kdePackages.isoimagewriter # Write hybrid ISOs to USB
  kdePackages.partitionmanager # Disk and partition management
  hardinfo2 # System benchmarks and hardware info
  wayland-utils # Wayland diagnostic tools
  wl-clipboard # Wayland copy/paste support
  vlc # Media player
];


	# services.displayManager.plasma-login-manager.enable = lib.mkIf (desktop == "kde") true;
# services.displayManager = {
#   plasma-login-manager.enable = true;
#   autoLogin.user = "user"; # Replace with the desired user
# };

services.keyd = {
  enable = true;
  keyboards.default = {
    # Match the devices specified in your original configuration
    ids = [
      "046d:c33c:2b17e9ba"
      "046d:c33c:e52c7974"
    ];

    settings = {
      global = {
        default_layout = "dvorak";
      };

      main = {
        apostrophe = "leftbrace";
        capslock = "overload(control,esc)";
        equal = "rightbrace";
        leftbrace = "minus";
        minus = "apostrophe";
        q = "z";
        rightbrace = "equal";
        z = "q";
      };
    };
  };
};
# systemd.services.keyd = {
#   wantedBy = [ "multi-user.target" ];
#   before = [ "display-manager.service" ];
# };
# services.keyd.enable = true;
# #deal with my custom key layout
# 	services.keyd = {
# 		enable = true;
# 		keyboards = {
#
# 			# default = {
# 			# 	ids = [ "*" ];
# 			# 	settings= {
# 			# 		main = {
# 			# 			capslock = "overload(control,esc)";
# 			# 		};
# 			# 	};
# 			# };
# # The name is just the name of the configuration file, it does not really matter
# 			default = {
# 				ids = [ "*" 
# 					# "046d:c33c:2b17e9ba"
# 					# "046d:c33c:e52c7974"
# 					# "046d:c33c:2b17e9ba"
#
# 				]; # The keyboard with swapped keys
# # Everything but the ID section:
# 				settings = {
# 					global= {
#
# 						default_layout = "dvorak";
# 					};
# # The main layer, if you choose to declare it in Nix
# 					main = {
# 						capslock = "overload(control,esc)";
# 						q = "z"; # you might need to also enclose the key in quotes if it contains non-alphabetical symbols
# 							z = "q";
# 						leftbrace="minus";
# 						rightbrace="equal";
# 						apostrophe="leftbrace";
# 						equal="rightbrace";
# 						minus="apostrophe";
#
# 					};
# 					otherlayer = {};
# 				};
# 				extraConfig = ''
# 					include layouts/dvorak
# # put here any extra-config, e.g. you can copy/paste here directly a configuration, just remove the ids part
# 					'';
# 			};
# 		};
# 	};
# #
# Configure keymap in X11
	services.xserver.xkb.layout = "us";
# services.xserver.xkb.variant="dvorak"; # or use keyd
# services.xserver.xkb.options = "caps:escape"; # or use keyd

# Enable CUPS to print documents.
	services.printing.enable = true;

# Enable sound.
# services.pulseaudio.enable = true;
# OR
	services.pipewire = {
		enable = true;
		pulse.enable = true;
	};

# Enable touchpad support (enabled default in most desktopManager).
	services.libinput.enable = true;




services.flatpak.enable = true;



# Define a user account. Don't forget to set a password with ‘passwd’.
	users.users.blake = {
		isNormalUser = true;
		shell=pkgs.zsh;
		extraGroups = [ "wheel" ];
		home="/home/blake";
		packages = with pkgs; [
			pkgs.tree 
				pkgs.cmake
				pkgs.wget
				pkgs.claude-code
				pkgs.curl
				pkgs.gcc
				pkgs.gnumake
				pkgs.spotify
				pkgs.ocaml
				pkgs.dune
				pkgs.eza
				pkgs.ocamlPackages.ocaml-lsp
				pkgs.ocamlPackages.utop
				pkgs.ocamlPackages.merlin
				pkgs.ocamlPackages.dot-merlin-reader
				pkgs.go
				pkgs.jdk
				pkgs.kotlin
				pkgs.elixir
				pkgs.prettier
				pkgs.nodejs
				pkgs.python3
				pkgs.pyright
				pkgs.clang
				pkgs.clang-tools
				pkgs.ripgrep
				pkgs.tree-sitter
				pkgs.bun
				pkgs.rustc
				pkgs.cargo
				pkgs.emacs
				pkgs.google-chrome
				pkgs.nixd
				pkgs.android-studio-full
				pkgs.heroic
				pkgs.postgresql
				pkgs.just
				pkgs.jdk
				pkgs.gradle
				pkgs.stow
				pkgs.kotlin
				pkgs.elixir
				pkgs.ghostty
				pkgs.galaxy-buds-client
				pkgs.jetbrains.clion
# pkgs.llvmPackages.libclang
				pkgs.pulseaudio
				];
	};

# # List packages installed in system profile.
# # You can use https://search.nixos.org/ to find more packages (and options).
# environment.systemPackages = with pkgs; [
#     neovim # Do not forget to add an editor to edit configuration.nix! The Nano editor is also installed by default.
# ];
nixpkgs.config.android_sdk.accept_license = true;


	programs.nixvim = {
		enable=true;
		imports = [./custards/init.nix];
# colorschemes.catpuccin.enable = true;
# plugins.lualine.enable = true;
	};
	

	# home-manager.users.blake.services.deconnect.enable = true;

networking.firewall = rec {
  allowedTCPPortRanges = [ { from = 1714; to = 1764; } ];
  allowedUDPPortRanges = allowedTCPPortRanges;
};
#xdg.portal = {
 # enable = true;
#  extraPortals = [ pkgs.kdePackages.xdg-desktop-portal-kde ]; # Use pkgs.libsForQt5.xdg-desktop-portal-kde if on Plasma 5
#};

		programs.kdeconnect.enable=true;
	programs.bash.enable = true;
	home-manager.users.blake = { pkgs, ... }: {
		programs.bash.enable = true;
		programs.firefox.enable = true;
		programs.gh = {
			enable=true;
			gitCredentialHelper.enable=true;
		};
		programs.git = {
			enable= true;
			settings={
				user={
					name = "Custards1";
					email = "blake.d.brown77@gmail.com";
				};
				init.defaultBranch = "main";
			};
		};
		nixpkgs.config.allowUnfree = true;
		programs.vscode.enable=true;
		home.sessionVariables = {
			EDITOR="nvim";
			BROWSER="firfox";
			TERMINAL="ghostty";
		};

# The state version is required and should stay at the version you
# originally installed.
		home.stateVersion = "25.11";
	};



	programs.zsh = {
		enable=true;
	};

# programs.neovim = {
# 	enable=true;
# 	defaultEditor=true;
# 	viAlias=true;
# 	vimAlias=true;
# 	configure={
# 	    customLuaRC=builtins.readFile neovimConfig;
# 	    packages.myVimPackage = with pkgs.vimPlugins; {
# 	    # loaded on launch
# 	    start = [ nvim-lspconfig nvim-treesitter telescope-nvim gruvbox ];
# 	    # manually loadable by calling `:packadd $plugin-name`
# 	    opt = [ ];
# 	  };
#
# 	};
# };

	programs.steam = {
		enable = true;
		remotePlay.openFirewall = true; # Open ports in the firewall for Steam Remote Play
			dedicatedServer.openFirewall = true; # Open ports in the firewall for Source Dedicated Server
			localNetworkGameTransfers.openFirewall = true; # Open ports in the firewall for Steam Local Network Game Transfers
	};
# programs.openmw.enable=true;

# Some programs need SUID wrappers, can be configured further or are
# started in user sessions.
# programs.mtr.enable = true;
# programs.gnupg.agent = {
#   enable = true;
#   enableSSHSupport = true;
# };

# List services that you want to enable:

# Enable the OpenSSH daemon.
	services.openssh.enable = true;
services.avahi = {
  enable = true;
  nssmdns4 = true;
  openFirewall = true;
};
nix.settings.experimental-features = [ "nix-command" "flakes" ];

# Open ports in the firewall.
# networking.firewall.allowedTCPPorts = [ ... ];
# networking.firewall.allowedUDPPorts = [ ... ];
# Or disable the firewall altogether.
# networking.firewall.enable = false;

# Copy the NixOS configuration file and link it from the resulting system
# (/run/current-system/configuration.nix). This is useful in case you
# accidentally delete configuration.nix.
# system.copySystemConfiguration = true;

# This option defines the first version of NixOS you have installed on this particular machine,
# and is used to maintain compatibility with application data (e.g. databases) created on older NixOS versions.
#
# Most users should NEVER change this value after the initial install, for any reason,
# even if you've upgraded your system to a new NixOS release.
#
# This value does NOT affect the Nixpkgs version your packages and OS are pulled from,
# so changing it will NOT upgrade your system - see https://nixos.org/manual/nixos/stable/#sec-upgrading for how
# to actually do that.
#
# This value being lower than the current NixOS release does NOT mean your system is
# out of date, out of support, or vulnerable.
#
# Do NOT change this value unless you have manually inspected all the changes it would make to your configuration,
# and migrated your data accordingly.
#
# For more information, see `man configuration.nix` or https://nixos.org/manual/nixos/stable/options#opt-system.stateVersion .
	system.stateVersion = "25.11"; # Did you read the comment?

		hardware.xone.enable=true;
	
}
