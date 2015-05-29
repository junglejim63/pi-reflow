var util = require("util");
var events = require("events");
var fs = require("fs");
var spawn = require('child_process').spawn;
var split = require('split');
var jf = require('jsonfile');
var pdc = require("./ProcessDataCompression.js");

//*******************************************
// Reflow object
//*******************************************
function Reflow() {
   events.EventEmitter.call(this);
   this.tc1 = {"item": "TC", "channel": 0, "n": 0, "M": -100, "S": 0, "value": -100, "stdev": 0, "max": -100, "min": -100};
   this.tc2 = {"item": "TC", "channel": 1, "n": 0, "M": -100, "S": 0, "value": -100, "stdev": 0, "max": -100, "min": -100};
   this.controller = {"item": "Controller", "beep_mode": 0, "mode": 0, "pv": -100, "out": 0, "sp": 20, 
                      "Kp": 1, "Ti": 60, "Td": 0, "Bias": 0, "outHi": 100, "outLo": 0, "SPHi": 300, "SPLo": 0,
                      "lastLastError": 0, "lastError": 0, "lastKp": 1, "lastTi": 60, "lastTd": 0, "lastintegral": 0,
                      "lastLooptime": 0, "lastMode": 0};
   this.pid_pwm = null;
   this.pid_stdin = null;
   this.cmdResponse = null;
   this.processMsg = null;
   //spawn controller process
   this.reflowProcess = null;
   //sequence parameters
   this.state = "Idle";
   this.stateID = -1;
   this.stateRamp = 0;
   this.stateStartTemp = 20;
   this.stateEndTemp = 20;
   this.stateStartMS = null;
   this.stateEndMS = null;
   this.update = {scanMS: 500, nextscanID:null, displaySecs: 3, lastMS: 0};
   this.tuningFile = "./config/tuning.json";
   this.sequencesFile = "./config/sequences.json";
   this.archiveDir = "./archive/";
   this.sequences = null;
   this.ipv4Addresses = [];
   this.run = {title: "", startMS: 0, endMS: 0, TAL: 0, cycleName: "", status: "Idle"};
   this.pvHist = new pdc.ProcessDataCompression({type: "BOXCARBACKSLOPE", tol: 1.0});
   this.outHist = new pdc.ProcessDataCompression({type: "BOXCARBACKSLOPE", tol: 1.0});
   this.runs = [];
   //from http://www.compuphase.com/electronics/reflowsolderprofiles.htm
   this.sequence = {name: "Lead (Sn63 Pb37)", liquidus: 219, order: ["Preheat", "Soak", "Heat", "Reflow", "Cool"],
            steps: {Preheat: {Tend: 150, Ramp: 3},
                    Soak: {Tend: 180, Duration: 120},
                    Heat: {Tend: 245, Ramp: 2},
                    Reflow: {Tend: 245, Duration: 10},
                    Cool: {Tend: 30,  Ramp: -4}}};
}
util.inherits(Reflow, events.EventEmitter);

//*******************************************
// Start method
//*******************************************
Reflow.prototype.start = function() {
   var me = this;
   me.reflowProcess = spawn('./reflowControl');
   me.reflowProcess.stdout.pipe(split()).on('data', function(data) {
      var tmp;
      try {
         tmp = JSON.parse(data);
         if (typeof tmp.item === "undefined") {
            console.log("Unexpected message: " + data);
            return;
         }
         switch (tmp.item) {
            case "Command":
               me.cmdResponse = tmp;
               me.cmdResponse.updateTimeMS = Date.now();
               me.emit('CommandUpdate');
               break;
            case "Controller":
               me.controller = tmp;
               me.controller.updateTimeMS = Date.now();
               me.emit('ControllerUpdate');
               break;
            case "TC":
               if (tmp.channel === 0) {
                  me.tc1 = tmp;
                  me.tc1.updateTimeMS = Date.now();
                  me.emit('TC1Update');
               } else {
                  me.tc2 = tmp;
                  me.tc2.updateTimeMS = Date.now();
                  me.emit('TC2Update');
               }
               break;
            case "childProcess":
               if (tmp.value == "PID_PWM") {
                  me.pid_pwm = tmp;
                  me.pid_pwm.updateTimeMS = Date.now();
                  me.emit('PWMUpdate');
               } else {
                  me.pid_stdin = tmp;
                  me.pid_stdin.updateTimeMS = Date.now();
                  me.emit('STDINUpdate');
                  me.startUpdate(me);
               }
               break;
            case "processMessage":
               me.processMsg = tmp;
               me.processMsg.updateTimeMS = Date.now();
               me.emit('ProcessUpdate');
               break;
            default:
               console.log("Unknown message from child process, item type: " + tmp.item)
               break;
         }
      } catch(err) {
         // console.log('stdout: ' + data);
         return
      }
   });
   me.reflowProcess.stderr.pipe(split()).on('data', function(data) {
       console.log('stderr: ' + data);
   });
   me.reflowProcess.on('close', function(code) {
       console.log('child process exited with code ' + code);
   });
   
   //Load default tuning parameters if file exists.  Wait 5 seconds after startup to make sure child process is ready
   setTimeout(function(me) {
      me.readSequences(me);
      me.readTuning(me);
   }, 5000, me);
   
};

//*******************************************
// Stop method
//*******************************************
Reflow.prototype.stop = function() {
   if (this.update.nextscanID !== null) {
      clearInterval(this.update.nextscanID);
   }
   try {
      process.kill(this.reflowProcess.pid, 'SIGTERM');
   } catch(err) {
      console.log("Child process has already terminated. Error: " + err);
   }
};

//*******************************************
// Start update task
//*******************************************
Reflow.prototype.startUpdate = function(me) {
   if (me.update.nextscanID !== null) {
      clearInterval(me.update.nextscanID);
   }
   me.update.nextscanID = setInterval(function(me) {
      me.updateTask.call(me);
   }, me.update.scanMS, me);
};

//*******************************************
// Update task
//*******************************************
Reflow.prototype.updateTask = function() {
   var nowMS = Date.now();
   if (this.stateID !== -1) {
      //compress PV
      this.run.pvHist = this.run.pvHist.concat(this.pvHist.scan({value: this.tc1.value, timestamp: this.tc1.updateTimeMS}));
      //compress out
      this.run.outHist = this.run.outHist.concat(this.outHist.scan({value: this.controller.out, timestamp: this.controller.updateTimeMS}));
      //calculate TAL
      if (this.controller.pv >= this.sequence.liquidus) {
         this.run.TAL += (nowMS - this.update.lastMS)/1000.0;
      }
      var left = remainingMS(this.stateEndMS);
      var sp = this.stateEndTemp - (this.stateEndTemp-this.stateStartTemp)*(left)/(this.stateEndMS-this.stateStartMS);
      this.newValues.call(this,{sp: sp}); //set controller setpoint
      if (left === 0) this.nextState.call(this);
      this.emit('RunUpdate');
   }
   this.emit('Status', {state: this.state, stateEndMS: this.stateEndMS, stateID: this.stateID});
   this.lcdStatus(this);
   this.update.lastMS = nowMS;
};

//*******************************************
// seqStart method
//*******************************************
Reflow.prototype.seqStart = function(title) {
   if (this.stateID !== -1) return;
   var d = new Date();
   title = title || d.toISOString();
   this.run = {title: title, startMS: Date.now(), endMS: null, TAL: 0, cycleName: this.sequence.name, status: "Running"};
   this.run.profile = this.profile(this.run.startMS, this.sequence);
   this.run.pvHist = this.pvHist.start({value: this.tc1.value, timestamp: this.tc1.updateTimeMS});
   this.run.outHist = this.outHist.start({value: this.controller.out, timestamp: this.controller.updateTimeMS});
   this.newValues({mode: 1}); //put controller in Auto
   this.nextState();
   this.emit('Run');
}

//*******************************************
// Profile - returns an array of temperature points in x,y pairs
//*******************************************
Reflow.prototype.profile = function(start, sequence) {
   var phaseEndMS, phaseEndTemp, duration, params;
   var phaseStartMS = start;
   var phaseStartTemp = this.controller.pv;
   var profile = [{timestamp: phaseStartMS, value: phaseStartTemp}];   
   for (var i = 0; i< sequence.order.length; i++) {
      params = sequence.steps[sequence.order[i]];
      phaseEndTemp = params.Tend;
      if (typeof params.Ramp !== 'undefined') {
         duration = (phaseEndTemp - phaseStartTemp)/params.Ramp*1000;
      } else {
         duration = params.Duration * 1000;
      }         
      phaseEndMS = phaseStartMS + Math.round(duration);
      phaseStartMS = phaseEndMS;
      phaseStartTemp = phaseEndTemp;
      profile.push({timestamp: phaseStartMS, value: phaseStartTemp});
   }
   return profile;
};

//*******************************************
// nextState method
//*******************************************
Reflow.prototype.nextState = function() {
   var duration;
   this.stateID += 1;
   if (this.stateID >= this.sequence.order.length) { //sequence is finished
      this.seqStop(true);
   } else {
      this.beep(1);
      var seq = this.sequence.order[this.stateID];
      this.state = seq;
      var params = this.sequence.steps[seq];
      this.stateStartTemp = this.controller.sp;
      this.stateEndTemp = params.Tend;
      if (typeof params.Ramp !== 'undefined') { //use a ramp temperature rate to determine end time
         duration = (params.Tend - this.stateStartTemp)/params.Ramp;
      } else {
         duration = params.Duration;
      }
      this.stateStartMS = Date.now();
      this.stateEndMS = this.stateStartMS + duration*1000;
   }
}

//*******************************************
// seqStop method
//*******************************************
Reflow.prototype.seqStop = function(complete) {
   complete = complete || false;
   this.state = "Idle";
   this.stateID = -1;
   this.stateRamp = 0;
   this.stateStartTemp = 20;
   this.stateEndTemp = 20;
   this.stateStartMS = null;
   this.stateEndMS = null;
   this.newValues({mode: 0, out: 0}); //put controller in Manual, output 0%
   this.beep(3);
   this.run.endMS = Date.now();
   this.run.status = (complete) ? "Complete" : "Aborted";
   //stop PV compression
   this.run.pvHist = this.run.pvHist.concat(this.pvHist.stop({value: this.tc1.value, timestamp: this.tc1.updateTimeMS}));
   //stop out compression
   this.run.outHist = this.run.outHist.concat(this.outHist.stop({value: this.controller.out, timestamp: this.controller.updateTimeMS}));
   this.emit('RunUpdate');
   this.archiveRun();
   this.runs.push(this.run);
}

//*******************************************
// Beep method
//*******************************************
Reflow.prototype.beep = function(beepmode) {
   this.reflowProcess.stdin.write('beep_mode '+ beepmode +'\n');
};

//*******************************************
// LCD method
//*******************************************
Reflow.prototype.lcd = function(text, line) {
   text = text || " ";
   line = (line == 0) ? "0" : "1";
   text = stringPad(text, " ", 16);
   this.reflowProcess.stdin.write('lcd'+ line + " " + text +'\n');
};
Reflow.prototype.lcd0 = function(text) {this.lcd(text,0);};
Reflow.prototype.lcd1 = function(text) {this.lcd(text,1);};

//*******************************************
// LCD status method - display status on the LCD based on current state
//*******************************************
Reflow.prototype.lcdStatus = function(me) {
   if (me.stateEndMS !== null) { //count down to end time
      var timeTxt = formatMS(remainingMS(me.stateEndMS));
      me.lcd0(me.state + " " + timeTxt);
      me.lcd1(("   "+me.tc1.value.toFixed(1)).substr(-5) + " SP:" + ("   "+me.controller.sp.toFixed(1)).substr(-5));
   } else { //idle state
      var msgs = 1+me.ipv4Addresses.length;
      var cycle = msgs*me.update.displaySecs*1000;
      var curMsg = Math.floor((Date.now()%cycle)/(me.update.displaySecs*1000));
      me.lcd0(me.state + "   "+me.tc1.value.toFixed(1).substr(-6) + "C");
      if (curMsg === 0) {
         var now = new Date();
         var timeTxt = now.toTimeString().substr(0,8);
         me.lcd1("Time: " + timeTxt);
      } else {
         me.lcd1(me.ipv4Addresses[curMsg - 1]);
      }
      // me.lcd0(me.state + " " + timeTxt);
      // me.lcd1("Temp: "+me.tc1.value.toFixed(1).substr(-5) + "°C");
      // me.lcd1("Temp: "+me.tc1.value.toFixed(1).substr(-5) + "C");
   }
};

//*******************************************
// Read sequences
//*******************************************
Reflow.prototype.readSequences = function(me) {
   jf.readFile(me.sequencesFile,{throws: false},function(err, obj){
      if (obj !== null) {
         me.sequences = obj;
         me.sequence = obj[obj.Cycles[0]];
      }
   });
};

//*******************************************
// Write sequences
//*******************************************
Reflow.prototype.writeSequences = function(me) {
   jf.writeFile(me.sequencesFile, me.sequences, function(err, obj){
      if (err !== null) {
         console.log("Unable to write sequences to file "+me.sequencesFile +", Error: "+err);
      }
   });
};

//*******************************************
// Select sequence
//*******************************************
Reflow.prototype.selectSequence = function(seq, me) {
   if (typeof me.sequences[seq] === 'undefined') {
      console.log("Unable to load sequence: "+seq);
   } else {
      me.sequence = me.sequences[seq];
   }
};

//*******************************************
// Read tuning parameters
//*******************************************
Reflow.prototype.readTuning = function(me) {
   jf.readFile(me.tuningFile,{throws: false},function(err, obj){
      if (obj !== null) {
         me.newValues(obj);
      }
   });
};

//*******************************************
// Write tuning parameters
//*******************************************
Reflow.prototype.writeTuning = function(me) {
   var params = {"Kp": me.controller.Kp, "Ti": me.controller.Ti, "Td": me.controller.Td, "Bias": me.controller.Bias, 
   "outHi": me.controller.outHi, "outLo": me.controller.outLo, "SPHi": me.controller.SPHi, "SPLo": me.controller.SPLo};
   jf.writeFile(me.tuningFile, params, function(err, obj){
      if (err !== null) {
         console.log("Unable to write tuning parameters to file "+me.tuningFile +", Error: "+err);
      }
   });
};

//*******************************************
// Change controller values
//*******************************************
Reflow.prototype.newValues = function(values) {  //values = {sp:0.0, out: 100.0} etc.
   var params = ['sp', 'out', 'mode', 'Kp', 'Ti', 'Td', 'Bias', 'outHi', 'outLo', 'SPHi', 'SPLo'];
   for (var i = 0; i < params.length; i++) {
      if (typeof values[params[i]] !== 'undefined') {
         this.reflowProcess.stdin.write(params[i]+' '+ values[params[i]] +'\n');
      }
   }
};

//*******************************************
// Archive the run
//*******************************************
Reflow.prototype.archiveRun = function() {
   var me = this;
   var filename = this.archiveDir+((new Date()).toISOString() +"_"+ this.run.title).replace(/([^a-z0-9]+)/gi, '-')+".json";
   //console.log("Archive Filename: "+filename);
   jf.writeFile(filename, this.run, function(err, obj){
      if (err !== null) {
         console.log("Unable to write run results to file "+filename +", Error: "+err);
      } else {
         me.archiveDirList();
      }
   });
};

//*******************************************
// Get archive directory list
//*******************************************
Reflow.prototype.archiveDirList = function() {
   var me = this;
   fs.readdir(this.archiveDir, function (err, files){
      me.emit('ArchiveDirList', files);
   });
};

//*******************************************
// Read Archive file
//*******************************************
Reflow.prototype.archiveRead = function(filename) {
   var me = this;
   jf.readFile(this.archiveDir+filename,{throws: false},function(err, obj){
      if (obj !== null) {
         me.emit('ArchiveFile', {filename: filename, run: obj});
      } else {
         console.log("unable to read archive file "+me.archiveDir+filename);
      }
   });
};

//*******************************************
// Utility functions
//*******************************************
//stringPad - pads a string to len characters with pad character
function stringPad(txt, pad, len) {
   txt = txt + "";
   len = len || 16;
   pad = pad || " ";
   txt = txt.substr(0,len);
   var startlength = txt.length;
   for (var i = 0; i < (len - startlength); i++) {
      txt += pad;
   }
   return txt;   
}
//remainingMS - time remaining before a ms time in the future, 0 if in the past
function remainingMS(then) {
   return (Math.max(then-Date.now(), 0));
}
//formatMS - formats a ms time duration as DDD HH:MM:SS or HH:MM:SS if less than a day
function formatMS(duration) {
   var day = 24*60*60*1000;
   var hour = 60*60*1000;
   var minute = 60*1000;
   var result = (duration > day) ? Math.floor(duration/day) + " " : ""; //days if necessary
   duration = duration % day;
   result += ('00'+ Math.floor(duration/hour)).substr(-2) + ":"; //hours
   duration = duration % hour;
   result += ('00'+ Math.floor(duration/minute)).substr(-2) + ":"; //minutes
   duration = duration % minute;
   result += ('00'+ Math.floor(duration/1000)).substr(-2);  //seconds
   return result;
}

  
module.exports.Reflow = Reflow;
