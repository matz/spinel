# `class << Config; prepend G; end` written inside another body, with Config
# a module that has class methods of its own: the prepend stands ahead of
# them from where it runs, as with a class. A call before it reaches the
# module's own method.
module Config
  def self.hello = "own"
  def self.plain = "plain"
  def self.twice = yield + yield
end
module G
  def hello = "G:" + super
end
module H
  def hello = "H:" + super
  def twice = super { yield + "!" }
end
module Extra
  def extra = "extra"
end
class App
  def self.setup
    Config.hello
  end
  p setup
  class << Config
    prepend G
    include Extra
  end
  p setup, Config.extra
  class << Config
    prepend H
  end
  p setup, Config.plain, Config.twice { "t" }
end
p Config.hello
