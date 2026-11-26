# A writer call the method_missing rewrite holds the value of, and then
# does not split, runs in its scope: the compile went on to this refusal
# (a block's `self.title` on a class) instead of ending with SIGSEGV.
class Run
  def initialize(user)
  end
end
class Record
  def self.ops
    @ops ||= {}
  end
  def self.exposes
  end
  def self.rows
    @rows ||= []
  end
  def self.column_names
  end
  def self.columns(*names)
  end
  def self.initial(state = nil)
  end
  def self.define(kind, name, options, body)
    ops[name] = {
    }
  end
  def self.command(name, **options, &body)
  end
  def self.event(name, **options, &body)
  end
  def self.create(**options, &body)
    define(:create, :create, options, body)
  end
  def self.expose(audience, *names)
  end
  def self.reach(name, seen = [])
    op[:calls].each do |called|
    end
    ops.each do |name, op|
    end
  end
  def self.request(audience, user, name, id, args)
    saved = STORE.map { |model| model.rows.map { |row| row.dup } }
    begin
      if name == :create
      end
      STORE.each_with_index { |model, i| model.rows.replace(saved[i]) }
    end
  end
  def self.insert(attrs)
    row = attrs.merge(id: rows.size + 1)
    rows << row
  end
  def initialize(attrs, run, args)
  end
  def perform(name, args)
    if op[:from] && !op[:from].include?(@attrs[:state])
    end
  end
  def method_missing(name, *args, **keywords)
    if self.class.ops[name]
      if outer && !self.class.ops[outer][:calls].include?(name)
      end
    end
  end
end
class Reply < Record
end
class Ticket < Record
  command :sneaky do
  end
end
class Draft < Record
  create title: :string, calls: :publish do
    draft = insert(title: self.title)
  end
end
STORE = [Ticket, Reply, Draft]
def report(found)
end
Ticket.rows << { id: 1, state: :solved, requester_id: 42 }
begin
end
Ticket.rows[0][:state] = :open
begin
end
