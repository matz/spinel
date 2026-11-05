# The `except:` Array keeps its String elements although `run`'s `steps`,
# read off an element of a list nothing fills, may hold an untracked container.
class Route
  attr_reader :key, :verb, :steps
  def initialize(verb)
    @steps = []
  end
end

class Area
  attr_reader :except
  def initialize(prefix, except)
    @except = except
  end
end

class Routes
  def initialize
    @routes = []
    @areas = []
  end

  def use(prefix, except: [])
    @areas << Area.new(prefix, except)
    nil
  end

  def check
    @areas.each do |area|
      area.except.each do |key|
        next if @routes.any? { |r| r.key == key }
      end
    end
  end

  def call(req, res)
    route = nil
    allow = ""
    @routes.each do |r|
      allow = r.allow
      route = r if r.verb == req.method
    end
    if route
      run(route.steps, req, res)
      res.set_header("allow", allow)
    end
  end

  def run(steps, req, res)
    steps.each { |step| step.guard.check(req, res) }
  end

  def excepted
    @areas.map { |area| area.except.join(", ") }
  end
end

routes = Routes.new
routes.use "/api", except: ["POST /api/login", "GET /api/health"]
puts routes.excepted
