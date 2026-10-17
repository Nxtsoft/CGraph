class Cgraph < Formula
  desc "Code graph and change-context engine for coding agents"
  homepage "https://github.com/Nxtsoft/CGraph"
  version "0.9.5"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.5/cgraph-macos-arm64.tar.gz"
      sha256 "3d9bf5a7e4f5bea3aef7a12a849d5cd1adcb0aa5b9b31e8997d1612aa1811348"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.5/cgraph-macos-x64.tar.gz"
      sha256 "5547c611ba21902fcb2384974d12f68fe842e117036f4557f55d1f1176bea368"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.5/cgraph-linux-arm64.tar.gz"
      sha256 "cefd3861ba31fbbb5d7991bda9025ef396e9bcae2523c383f20b112137144518"
    end
    on_intel do
      url "https://github.com/Nxtsoft/CGraph/releases/download/bin-v0.9.5/cgraph-linux-x64.tar.gz"
      sha256 "7d18569529e4917c8b21244a67e25bdeb6b77acf34ddcda5111ed9b9222c3da8"
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
