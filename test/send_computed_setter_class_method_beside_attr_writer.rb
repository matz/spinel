# A computed setter send whose receiver may be a Class object with its own
# `self.request=`, an object with a `request=` method or one with an
# attr_writer answers the Hash it stored from each arm.
class Cur
  def self.request=(v)
    @r = v
  end
end
class Ctl
  def request=(v)
    @request = v
  end
end
class Conn
  attr_accessor :request
end
def run(record, subject, h) = record.__send__(subject, h)
def run_kw(record, subject, h) = record.__send__(subject, **h)
recs = [Cur, Ctl.new, Conn.new]
recs.each { |r| p run(r, :request=, { k: 1 }) }
recs.each { |r| p run_kw(r, :request=, { k: 2 }) }
p recs[2].request
