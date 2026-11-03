# RubyGems, as far as a compiled program can have it. CRuby starts with
# RubyGems loaded and library code asks it platform questions without a
# require (`Gem.win_platform?`, `Gem::Version.new(v) >= ...`). A compiled
# program has no gem machinery; these are the answers such code needs. The
# parser splices this file when the program names `Gem` and defines no Gem
# of its own.
module Gem
  VERSION = "4.0.20"
  class LoadError < ::LoadError; end
  class MissingSpecError < LoadError; end

  def self.win_platform? = false
  def self.java_platform? = false
  def self.ruby_version = Version.new(RUBY_VERSION)
  def self.platforms = []
  # no gem is activated in a compiled program, so no spec is loaded
  def self.loaded_specs = {}
  # no gem can provide a path: a compiled program has no gem to activate
  def self.try_activate(path) = false

  # Gem::Version: the runs of digits and of letters ("1.0.rc10" is
  # 1, 0, "rc", 10), numeric ones compared as numbers, a string segment (a
  # prerelease) sorting before any number -- RubyGems' own segmentation.
  class Version
    include Comparable
    attr_reader :segments

    def initialize(v)
      @version = v.to_s.strip
      @segments = @version.scan(/[0-9]+|[a-z]+/i).map { |s| s.match?(/\A\d+\z/) ? s.to_i : s }
    end

    def self.create(v) = new(v)
    def self.correct?(v) = v.to_s.strip.match?(/\A\d+(\.[0-9a-zA-Z]+)*\z/)

    def to_s = @version
    def version = @version
    def prerelease? = @segments.any? { |s| s.is_a?(String) }

    def <=>(other)
      o = other.is_a?(Version) ? other : Version.new(other.to_s)
      a = @segments
      b = o.segments
      n = a.size > b.size ? a.size : b.size
      i = 0
      while i < n
        x = i < a.size ? a[i] : 0
        y = i < b.size ? b[i] : 0
        if x.is_a?(Integer) && y.is_a?(Integer)
          return x < y ? -1 : 1 if x != y
        elsif x.is_a?(String) && y.is_a?(String)
          return x.to_s < y.to_s ? -1 : 1 if x != y
        else
          return x.is_a?(String) ? -1 : 1
        end
        i += 1
      end
      0
    end
  end
end
