# frozen_string_literal: true
# A receiverless `require` or `require_relative` inside a method of a class
# that defines the method calls the class's own method (boxcar's
# Controller::Parameters#expect called `require(key)`)
class Params
  def initialize(h)
    @h = h
  end

  def require(key)
    @h.fetch(key)
  end

  def expect(key)
    require(key)
  end

  def expect_name
    require("name")
  end
end

class Child < Params
  def pick(key)
    require(key)
  end
end

class Loader
  def self.require_relative(path)
    "loaded #{path}"
  end

  def self.load_part(part)
    require_relative(part)
  end
end

class JsonLoader
  def require(path)
    [path.frozen?, Kernel.require(path)]
  end

  def go
    require("json")
  end
end

class NulLoader
  def require(path)
    path.bytes
  end

  def go
    require("a\0b")
  end
end

p Params.new({room: 1}).require(:room)
p Params.new({room: 2}).expect(:room)
p Params.new({"name" => "x"}).expect_name
p Child.new({k: 3}).pick(:k)
p Loader.load_part("a/b")
p JsonLoader.new.go
p JSON.generate([1])
p NulLoader.new.go
