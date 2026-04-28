# Test module constants and top-level constants

MAX_SIZE = 100
PI_APPROX = 3
NAME = "spinel"

module Config
  VERSION = 42
  GREETING = "hello"
  ENABLED = true
end

puts MAX_SIZE         # 100
puts PI_APPROX        # 3
puts NAME             # spinel
puts Config::VERSION  # 42
puts Config::GREETING # hello
puts Config::ENABLED  # true

# Constants in expressions
puts MAX_SIZE + 1     # 101
puts NAME.length      # 6
puts Config::GREETING.upcase  # HELLO

# Lexical constant reads inside module/class initializers
module LexicalConst
  A = 10
  B = A + 1

  class C
    X = 7
    Y = X + 2
  end

  module N
    P = A + 3
  end
end

puts LexicalConst::B     # 11
puts LexicalConst::C::Y  # 9
puts LexicalConst::N::P  # 13

puts "done"
