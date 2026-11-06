# `class << K` written in another class's or module's body, K a class the
# program declares (#10790): the load record takes the body's include and
# prepend as K's singleton's where the statement runs, as it takes the
# top-level form; a module whose own singleton method the prepend would
# shadow keeps the refusal
module Logged
  def run = "logged " + super
end
module Named
  def label = "svc"
end
class Svc
  def self.run = "run"
end
module Setup
  class << Svc
    prepend Logged
    include Named
  end
  p Svc.run
end
class Other
  class << Svc
    def extra = :extra
  end
end
p Svc.run, Svc.label, Svc.extra
