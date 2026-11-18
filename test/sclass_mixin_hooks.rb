# Mixin hooks defined in `class << self`, as version_gem's Basic writes
# `extended`: each runs as `def self.<hook>` would, and the singleton
# class's other methods stay there.
module Api
  def major = self::VERSION.split(".").first
end

module Basic
  class << self
    def extended(base)
      raise ArgumentError, "VERSION must be defined" unless defined?(base::VERSION)
      base.extend(Api)
    end

    def label = "basic"
  end
end

module Edition
  VERSION = "7.1.0"
  extend Basic
end

module Plug
  module ClassMethods
    def plugged = "plugged #{name}"
  end

  class << self
    private

    def included(base)
      base.extend(ClassMethods)
    end
  end
end

class Thing
  include Plug
end

module Loud
  def shout = "#{super}!"
end

module Wrap
  class << self
    def prepended(base)
      base.include(Loud)
    end
  end
end

class Word
  def shout = "hi"
  prepend Wrap
end

p Edition.major
p Basic.label
p Thing.plugged
p Word.new.shout
