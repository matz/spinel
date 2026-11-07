# A constant aliasing a class is the class as `===`'s receiver: roundhouse's
# main.rb asks `FrameworkParseError === error` of a rescued exception.
module Params
  class ParseError < StandardError
  end
end

class BadRequest < StandardError
end

class FancyParseError < Params::ParseError
end

module Main
  FrameworkParseError = Params::ParseError

  def self.run(n)
    begin
      raise BadRequest, "b" if n == 0
      raise Params::ParseError, "p" if n == 1
      raise FancyParseError, "f" if n == 2
      1
    rescue BadRequest, Params::ParseError => error
      return "reraised #{error.class}" if FrameworkParseError === error && error.class != FrameworkParseError
      p FrameworkParseError === error
      400
    end
  end
end

p Main.run(0)
p Main.run(1)
p Main.run(2)
p Main.run(3)
