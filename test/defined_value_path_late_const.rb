# `defined?(base::VERSION)` on a class value, in a method defined before the
# classes that assign the constant: the run time answers, as CRuby does.
# version_gem's hook guards with it (`raise … unless defined?(base::VERSION)`).
module Probe
  def self.has?(base) = defined?(base::VERSION) ? "yes" : "no"

  def self.label(base) = defined?(base::VERSION)

  def self.require_version(base)
    raise ArgumentError, "VERSION must be defined" unless defined?(base::VERSION)
    "ok"
  end
end

module WithV
  VERSION = "1"
end

module WithoutV
end

class Klass
  VERSION = "k"
end

class Sub < Klass
end

p Probe.has?(WithV), Probe.has?(WithoutV), Probe.has?(Klass), Probe.has?(Sub)
p Probe.label(WithV), Probe.label(WithoutV)
p Probe.require_version(Klass)
begin
  Probe.require_version(WithoutV)
rescue ArgumentError => e
  p e.message
end

# A constant Ruby itself defines stays the run time's to answer.
def version?(base) = defined?(base::RUBY_VERSION) ? "yes" : "no"
p version?(Object), version?(WithV)
