# Modules a class's reopening includes stand in front of one another, each
# copy's super reaching the one included before it. A copy an include in a
# reopening makes is guarded until the include has run, and stands under a
# hidden name; when the next include renamed it, its super's chain entry
# stayed under the old hidden name, and from the third include on the
# middle module's super raised "no superclass method". actionpack's
# ActionController::Base, opened early and reopened with its includes,
# chains Instrumentation#render, ActionController::Rendering#render and
# AbstractController::Rendering#render this way.
module A
  def render(x) = "A #{x}"
end
module B
  def render(x) = "B(#{super})"
end
module C
  def render(x) = "C(#{super})"
end
module D
  def render(x) = "D(#{super})"
end

class Three; end
class Three
  include A
  include B
  include C
end
puts Three.new.render(1)

class Four; end
class Four
  include A
  include B
  include C
  include D
end
puts Four.new.render(2)

# the first include in the first body, the rest in a reopening
class Split
  include A
end
class Split
  include B
  include C
end
puts Split.new.render(3)

# a call between two reopenings sees the modules included so far
class Staged; end
class Staged
  include A
  include B
end
puts Staged.new.render(4)
class Staged
  include C
  include D
end
puts Staged.new.render(5)
