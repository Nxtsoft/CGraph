class Cgraph < Formula
  desc "Code graph and change-context engine for coding agents"
  homepage "https://github.com/Nxtsoft/CGraph"
  version "0.9.7"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.7/cgraph-macos-arm64.tar.gz"
      sha256 "e12322ce780b45ffa0264758a325bab956aaa57658142d9db801cbecf688e4d4"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.7/cgraph-macos-x64.tar.gz"
      sha256 "db7cc6c36721f0c5607d714ac7a1471cb894481474002df881d0ddd6ae79b27f"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.7/cgraph-linux-arm64.tar.gz"
      sha256 "e640d5966c8c27e691c14f49e7377d34b834ae0c86e4951b4253b112c410accf"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.7/cgraph-linux-x64.tar.gz"
      sha256 "8fd38c25b1d02a6659e55b5fb4a63b841a92cb13d6a5892d8777aea84a2ac924"
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
