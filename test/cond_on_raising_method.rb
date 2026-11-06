# A condition on a call to a method whose every path raises (a stub kept for
# a call that should not run, `template_exists?` in Rails' ImplicitRender
# when the action exists): it raises when reached, as in CRuby.

module Paths
  def exists?(*, **, &) = (raise(NotImplementedError, "not here"))
end

class Base
  def pick(name) = (name == "a" ? name : nil)
end

class Ctl < Base
  include Paths

  def pick(name)
    super || if exists?(name.to_s, [])
               "default"
             end
  end

  def guard(x)
    return :skipped unless exists?(x)
    :kept
  end
end

p Ctl.new.pick("a")
begin
  p Ctl.new.pick("b")
rescue NotImplementedError => e
  p e.message
end
begin
  p Ctl.new.guard(1)
rescue NotImplementedError => e
  p e.message
end
