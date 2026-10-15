class Cgraph < Formula
  desc "Code graph and change-context engine for coding agents"
  homepage "https://github.com/Nxtsoft/CGraph"
  version "0.9.4"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.4/cgraph-macos-arm64.tar.gz"
      sha256 "85fc1f405ae6422da2f7a45a3c91c4eb86bbf941904a1736cc3374750ed5c2cb"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.4/cgraph-macos-x64.tar.gz"
      sha256 "8d51a166822844227d3956e62ac37bab1910b9b9d1277e2933be2ae111aca84e"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.4/cgraph-linux-arm64.tar.gz"
      sha256 "6d3bec52d452265f80765ad07a0887f72553008d8945201679dea1bbc7bd4910"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.4/cgraph-linux-x64.tar.gz"
      sha256 "1eb29683ca2b85a05f6c8af9af69806d222e9483fd20dc3ed5fb10998d3c3869"
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
