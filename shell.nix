{ pkgs ? import <nixpkgs-unstable> {} }:

pkgs.mkShell {
  name = "Scylla trace viewer";
  buildInputs = with pkgs; [
    imgui
    glfw
    SDL2
    gnumake
    fmt
  ];

  IMGUI_DIR = pkgs.imgui;
}
