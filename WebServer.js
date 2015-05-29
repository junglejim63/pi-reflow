var util = require("util");
var events = require("events");
var events = require("events");
var fs = require('fs');
var mime = require('mime');
// var app = require('http').createServer(handler);
// var io = require('socket.io').listen(app);

var me = null;

//*******************************************
// WebServer object
//*******************************************
function WebServer(socket, root) {
   me = this;
   events.EventEmitter.call(this);
   this.socket = socket;
   this.root = root;
   this.app = require('http').createServer(handler);
   this.io = require('socket.io')(this.app);

   console.log("Starting server on port: "+this.socket);
   this.app.listen(this.socket);
}
util.inherits(WebServer, events.EventEmitter);

//*******************************************
// http request handler
//*******************************************
function handler(req, res) {
   var file = me.root + req.url;
   if (file.match(/\/$/)) {file+= "index.html";}
   if (file.match(/\.\./)) { //reject any file request with ".." in the url - protects from ascending the directory hierarchy
      res.writeHead(500);
      res.end('Unable to load '+ req.url);
   }
   fs.readFile(file, function(err, data) {
      if (err) {
         res.writeHead(500);
         res.end('Unable to load '+ req.url);
      } else {
         res.setHeader("Content-Type", mime.lookup(file)); //See http://stackoverflow.com/questions/11971918/how-do-i-set-a-mime-type-before-sending-a-file-in-node-js
         res.writeHead(200);
         res.end(data);
      }
   });
}

module.exports.WebServer = WebServer;
