var os = require('os');
var net = require("net");
var repl = require("repl");
var jf = require('jsonfile');
var r = require('./Reflow.js');
var w = require('./WebServer.js');

//Start the c program handling low level IO for thee reflow oven (reflowControl.c)
var reflow = new r.Reflow();
reflow.start();

//Start web server
var http = new w.WebServer(8000,__dirname+'/web');

//Create socket.io handlers
http.io.on('connection', function (socket) {
   socket.on('Run', function () {
      socket.emit('Run', reflow.run);
   });
   socket.on('SeqStart', function (title) {
      reflow.seqStart(title);
   });
   socket.on('SeqStop', function () {
      reflow.seqStop(false);
   });
   socket.on('SaveTuning', function () {
      console.log("save tuning");
      reflow.writeTuning(reflow);
   });
   socket.on('LoadTuning', function () {
      console.log("load tuning");
      reflow.readTuning(reflow);
   });
   socket.on('Sequences', function () {
      socket.emit('Sequences', reflow.sequences);
   });
   socket.on('Sequence', function () {
      socket.emit('Sequence', reflow.sequence);
   });
   socket.on('ReadArchiveFile', function (filename) {
      reflow.archiveRead(filename);
   });
   socket.on('SetSequence', function (index) {
      console.log("set Sequence: "+index);
      try {
         reflow.sequence = reflow.sequences[reflow.sequences.Cycles[index]];
         socket.emit('Sequence', reflow.sequence);
      } catch (e) {
         console.log("error setting sequence, index: "+index);
      }
   });
   socket.on('SetControllerValue', function (obj) {
      try {
         console.log("set controller value");
         console.dir(obj);
         reflow.newValues(obj);
      } catch (e) {
         console.log("error setting controller values: "+e);
      }
   });
   reflow.archiveDirList(); //Get the archive directory listing when a new connection comes in
});

//Create reflow event handlers
reflow.on('CommandUpdate', function() {
   http.io.emit('CommandUpdate', reflow.cmdResponse);
});
reflow.on('Run', function() {
   http.io.emit('Run', reflow.run);
});
reflow.on('ControllerUpdate', function() {
   http.io.emit('ControllerUpdate', reflow.controller);
});
reflow.on('TC1Update', function() {
   http.io.emit('TC1Update', reflow.tc1);
});
reflow.on('Status', function(status) {
   http.io.emit('Status', status);
});
reflow.on('ArchiveDirList', function(fileList) {
   http.io.emit('ArchiveDirList', fileList);
});
reflow.on('ArchiveFile', function(archive) {
   http.io.emit('ArchiveFile', archive);
});
reflow.on('RunUpdate', function() {
   http.io.emit('RunUpdate',
      {title: reflow.run.title, startMS: reflow.run.startMS, endMS: reflow.run.endMS, TAL: reflow.run.TAL, 
       cycleName: reflow.run.cycleName, status: reflow.run.status});
});



// Create signal handler so the C program can be cleanly closed
process.on('SIGINT', function() {
  safeEnd('SIGINT');
});
process.on('SIGTERM', function() {
  safeEnd('SIGTERM');
});
function safeEnd(signal) {
   console.log('Received '+signal+'.  Shutting down Pi outputs in 1 second...');
   try {
      reflow.stop()
   } catch(err) {
      console.log("Child process has already terminated.");
   }
   setTimeout(function() {
      console.log('Exiting.');
      process.exit(0);
      }, 1000);

}

//Create a remote node REPL instance so you can telnet directly to this server
net.createServer(function (socket) {
  var remote = repl.start("reflow::remote> ", socket);
  //Adding objects to the remote REPL's context.
  remote.context.reflow = reflow;
  remote.context.http = http;
}).listen(5001);

console.log("Remote REPL started on port 5001.");

//A "local" node repl with a custom prompt
var local = repl.start("reflow::local> ");

//Adding objects to the local REPL's context.
local.context.reflow = reflow;
local.context.http = http;

//*****************************************************
// Get ethernet network interfaces and send to reflow object
//*****************************************************
function getIP() {
   var addresses = [];
   var ifaces = os.networkInterfaces();
   Object.keys(ifaces).forEach(function (ifname) {
      var alias = 0;
      ifaces[ifname].forEach(function (iface) {
         if ('IPv4' !== iface.family || iface.internal !== false) {
            // skip over internal (i.e. 127.0.0.1) and non-ipv4 addresses
            return;
         }
         addresses.push(iface.address);
      });
   });
   return addresses;
}
reflow.ipv4Addresses = getIP();