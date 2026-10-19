class Cgraph < Formula
  desc "Code graph and change-context engine for coding agents"
  homepage "https://github.com/Nxtsoft/CGraph"
  version "0.9.6"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.6/cgraph-macos-arm64.tar.gz"
      sha256 "55414b665fd8dcf98dd700c9034a5ebac8e2e19e8c4e2e106bbf56310c810cac"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.6/cgraph-macos-x64.tar.gz"
      sha256 "7dea72c4d848adce94fb5931c3964760d451a9a2b22daa30645987fc2514dc5e"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.6/cgraph-linux-arm64.tar.gz"
      sha256 "f970a332e46918fe2358077826f48af1badafd6351cce68bc2c21dd90e27d296"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.6/cgraph-linux-x64.tar.gz"
      sha256 "afdcb4da4f6bc42ec9a28d55a29cab068413f9991e7a96a0c54098ac07cca5cc"
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
