# A Hash passed to an --rbs-seeded Hash parameter, in a program with a
# send of a computed name: the seeded parameter may hold an untracked
# container, and its sites still stay out of the alias groups (#11133).
module SeedSendCatalog
  def self.load
    table = {}
    "greeting=hello".split(";").each do |line|
      fields = line.split("=")
      table[fields[0]] = fields[1].to_s
    end
    @table = table
    nil
  end

  def self.table
    load if @table.nil?
    @table || {}
  end

  def self.fetch_in(table, key)
    table.fetch(key, nil)
  end

  def self.lookup(key)
    table = self.table
    fetch_in(table, key)
  end

  def self.noop(x) = x
end

name = "noop"
SeedSendCatalog.send(name, 1)
puts SeedSendCatalog.lookup("greeting")
