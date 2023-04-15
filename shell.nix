{ pkgs ? import <nixpkgs> {} }:

#(pkgs.overrideCC pkgs.stdenv (pkgs.ccacheWrapper.override { cc = pkgs.clang_15; })).mkDerivation {
pkgs.ccacheStdenv.mkDerivation {
  name = "Scylla trace viewer";
  buildInputs = with pkgs; [
    ccache
    imgui
    glfw
    SDL2
    gnumake
    fmt
  ];

  IMGUI_DIR = pkgs.imgui;
}
