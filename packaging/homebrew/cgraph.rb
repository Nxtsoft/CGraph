class Cgraph < Formula
  desc "Code graph and change-context engine for coding agents"
  homepage "https://github.com/Nxtsoft/CGraph"
  version "0.9.8"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.8/cgraph-macos-arm64.tar.gz"
      sha256 "7a5d1f4311eb93cc6413aa97cae6273e7365479d0662fad194518785c2d7029f"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.8/cgraph-macos-x64.tar.gz"
      sha256 "3c50135e61754169aae42329992c287b23a9235a0ae934e20a31a5e920db4c59"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.8/cgraph-linux-arm64.tar.gz"
      sha256 "5dff84b64c59fe9368068b306b28d6944c75f18ca3d5700eb3b525f384cf6bef"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.8/cgraph-linux-x64.tar.gz"
      sha256 "a21cc2c6a333d5e5e56acdc2767a952fa7207ae7f3f44d3e8613e51ece115ea4"
    end
  end

  def install
    bin.install "cgraph", "graphd", "cgraph-client", "cgraph-mcp"
  end

  test do
    (testpath/"app.py").write "def alpha():\n    return 1\n"
    answer = shell_output("#{bin}/cgraph-client --root #{testpath} query '{\"query\": \"alpha\"}'")
    assert_match "alpha", answer
    system bin/"cgraph-client", "--root", testpath, "shutdown"
  end
end
