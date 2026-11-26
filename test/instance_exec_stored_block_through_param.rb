# A block a class body stores, handed to instance_exec through a method's
# parameter or a local, runs on the receiver: its bare calls reach the
# receiver's methods.
class Record
  def self.bodies
    @bodies ||= {}
  end

  def self.body(name, &block)
    bodies[name] = block
  end

  def label = "plain"
  def update(changes) = changes

  def run_body(body)
    self.instance_exec(&body)
  end

  def run(name)
    run_body(self.class.bodies[name])
  end

  def run_local(name)
    b = self.class.bodies[name]
    instance_exec(&b)
  end

  body(:read) { label }
  body(:shout) { label.upcase }
  body(:own) { self.label }
  body(:write) { update(state: :sent) }
end

r = Record.new
p r.run(:read), r.run(:shout), r.run(:own), r.run(:write)
p r.run_local(:read), r.run_local(:write)
