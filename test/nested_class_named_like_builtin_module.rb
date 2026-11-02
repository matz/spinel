# A class nested in a module under a builtin MODULE's name (solid_queue's
# `SolidQueue::Process`) is a constant of its own: it cannot reopen the
# builtin module, so the rooted `::Process.pid` beside it still names the
# builtin, while a bare `Process` inside the module names the nested class.
module Queue2
  class Process
    def self.register(pid)
      "registered #{pid > 0}"
    end
  end

  class GC
    def self.label = "Queue2::GC"
  end

  def self.worker = Process.register(::Process.pid)
  def self.gc = [GC.label, ::GC.count >= 0]
end

module Runner
  def self.pid = ::Process.pid
end

puts Queue2.worker
p Queue2.gc
p Runner.pid == Process.pid
p Queue2::Process.register(1)
