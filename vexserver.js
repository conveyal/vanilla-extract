/* A node server to stream vex output
   usage: node vexserver.js (on ubuntu, nodejs vexserver.js)
   Parameters are taken from the environment:
    VEX_DB: the path to the vex database to use, default '/var/osm/db/'
    VEX_CMD: the command to run vex, default 'vex'
    VEX_HOST: the hostname to bind on, or 0.0.0.0 for all interfaces; default 0.0.0.0
    VEX_PORT: the port to server on, default 8282
*/

var http = require('http');
var url = require('url');
var spawn = require('child_process').spawn;

var dbname = process.env.VEX_DB || '/var/osm/db';
var port = process.env.VEX_PORT || 8282;
var host = process.env.VEX_HOST || '0.0.0.0';
var cmd  = process.env.VEX_CMD  || 'vex';

var vex = function (req, res) {
  var query = url.parse(req.url, true).query;

  var north = Number(query.north);
  var south = Number(query.south);
  var east  = Number(query.east);
  var west  = Number(query.west);

  if (isNaN(north) || isNaN(south) || isNaN(east) || isNaN(west)) {
    res.writeHead(400, {'Content-Type': 'text/plain'});
    res.end('Usage: ?north=<lat>&south=<lat>&east=<lon>&west=<lon>');
    return;
  }

  if (north <= south || east <= west) {
    res.writeHead(400, {'Content-Type': 'text/plain'});
    res.end('North must be north of south; east must be east of west');
    return;
  }

  if (north < -90 || north > 90 || south < -90 || south > 90) {
    res.writeHead(400, {'Content-Type': 'text/plain'});
    res.end('Latitudes must be between -90 and 90');
    return;
  }

  if (west < -180 || west > 180 || east < -180 || east > 180) {
    res.writeHead(400, {'Content-Type': 'text/plain'});
    res.end('Longitudes must be between -180 and 180');
    return;
  }

  // TODO: error checking within vex?
  // TODO: does pbf have its own MIME type?
  res.writeHead(200, {
    'Content-Type': 'application/octet-stream',
    'Content-Disposition': 'attachment;filename=osm_export_' +
      ((north + south) / 2) + '_' + ((east + west) / 2) + '.pbf'
    });

  // run vex
  var proc = spawn(cmd, [dbname, south, west, north, east, '-']);

  // stream chunks, let OS do buffering
  proc.stdout.on('data', function (data) {
    res.write(data);
  })
  .on('end', function () {
    res.end();
  });
};

// start a server
http.createServer(vex).listen(port, host);
console.log('vex server running at ' + host + ':' + port);