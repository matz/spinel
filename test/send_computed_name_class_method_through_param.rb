# A computed send's arm on a value that may be a Class object reaches that
# class's singleton method of the name, as a send on the constant does.
class Job
  def self.go = :job
  def self.take(a, k: 0) = [a, k]
  def go = :obj
end
class Sub < Job
end
module Tool
  def self.go = :tool
end
class Idle
  def self.go = :idle
end
def run(r, s) = r.__send__(s)
def run_args(r, s, a, h) = r.__send__(s, a, **h)
p run(Job.new, :go)
p run(Job, :go)
p run(Sub, :go)
p run(Tool, :go)
p run(Sub, :name)
p run_args(Job, :take, 1, { k: 2 })
p run_args(Sub, :take, 3, {})
