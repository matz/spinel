# A define_method in a branch that never runs defines nothing in CRuby
# ("plain" twice), but it is registered from the start all the same.
class R
  def show = "plain"
  def self.install!
    if false
      define_method(:show) { "fancy" }
    end
  end
end
p R.new.show
R.install!
p R.new.show
