# A method every object has is replaced too: before `install!` runs, CRuby's
# own tap answers (it prints R, then 42).
class R
  def self.install! = define_method(:tap) { 42 }
end
p R.new.tap { }.class
R.install!
p R.new.tap
