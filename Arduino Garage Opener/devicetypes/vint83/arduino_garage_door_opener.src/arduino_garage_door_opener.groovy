/**
 *  Arduino Garage Door Opener
 *
 *  Copyright 2016 vint83
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 *  in compliance with the License. You may obtain a copy of the License at:
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software distributed under the License is distributed
 *  on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License
 *  for the specific language governing permissions and limitations under the License.
 *
 */

import java.security.MessageDigest;

preferences {
    input("ip", "text", title: "IP Address", description: "ip")
    input("port", "text", title: "Port", description: "port")
    input("mac", "text", title: "MAC Addr", description: "mac")
    input("password", "password", title: "Password", description: "password")
}

metadata {
    definition (name: "Arduino Garage Door Opener", namespace: "vint83", author: "vint83") {
        capability "Actuator"
        capability "Door Control"
        capability "Garage Door Control"
        capability "Contact Sensor"
        capability "Refresh"
        capability "Sensor"
        capability "Polling"
        capability "Temperature Measurement"
        
    }
    
    simulator {
        
    }
    
    tiles {
        standardTile("toggle", "device.door", width: 3, height: 2) {
            state("closed", label:'${name}', action:"open", icon:"st.doors.garage.garage-closed", backgroundColor:"#79b821", nextState:"opening")
            state("open", label:'${name}', action:"close", icon:"st.doors.garage.garage-open", backgroundColor:"#ffa81e", nextState:"closing")
            state("opening", label:'${name}', icon:"st.doors.garage.garage-closed", backgroundColor:"#ffe71e")
            state("closing", label:'${name}', icon:"st.doors.garage.garage-open", backgroundColor:"#ffe71e")
            state("unknown", label:'${name}', icon:"st.doors.garage.garage-unknown")
            
        }
        standardTile("open", "device.door", inactiveLabel: false, decoration: "flat") {
            state "default", label:'open', action:"open", icon:"st.doors.garage.garage-opening"
        }
        standardTile("close", "device.door", inactiveLabel: false, decoration: "flat") {
            state "default", label:'close', action:"close", icon:"st.doors.garage.garage-closing"
        }
        standardTile("refresh", "device.refresh", inactiveLabel: false, decoration: "flat") {
            state "default", label:'', action:"refresh.refresh", icon:"st.secondary.refresh"
        }
        valueTile("temperature", "device.temperatureCombined", decoration: "flat") {
            state "temperature", label:'${currentValue}'
        }
        
        main "toggle"
        details(["toggle", "open", "close", "temperature", "refresh"])
    }
}

// gets the address of the hub
private getCallBackAddress() {
    return device.hub.getDataValue("localIP") + ":" + device.hub.getDataValue("localSrvPortTCP")
}

// gets the address of the device
private getHostAddress() {
    def ip = settings.ip
    def port = settings.port
    
    if (!ip || !port) {
        def parts = device.deviceNetworkId.split(":")
        if (parts.length == 2) {
            ip = parts[0]
            port = parts[1]
            log.debug "Using IP: $ip and port: $port for device: ${device.id}"
            return convertHexToIP(ip) + ":" + convertHexToInt(port)
        } else {
            log.warn "Can't figure out ip and port for device: ${device.id}"
        }
    }
    
    log.debug "Using IP: $ip and port: $port for device: ${device.id}"
    return ip + ":" + port
}

private Integer convertHexToInt(hex) {
    return Integer.parseInt(hex,16)
}

private String convertHexToIP(hex) {
    return [convertHexToInt(hex[0..1]),convertHexToInt(hex[2..3]),convertHexToInt(hex[4..5]),convertHexToInt(hex[6..7])].join(".")
}

private String convertIPtoHex(ipAddress) {
    String hex = ipAddress.tokenize( '.' ).collect {  String.format( '%02x', it.toInteger() ) }.join()
    return hex
}

private String convertPortToHex(port) {
    String hexport = port.toString().format( '%04x', port.toInteger() )
    return hexport
}

def refresh() {
    poll()
}

def sendDoorUnknownEvent() {
    state.previousDoorState = "unknown"
    sendEvent(name: "door", value: "unknown", displayed: false)
}

def poll() {
    if(device.deviceNetworkId!=settings.mac) {
        log.debug "setting device network id to device MAC"
        device.deviceNetworkId = settings.mac;
    }
    state.previousDoorState = device.currentValue("door")
    log.debug "Executing 'poll'"
    runIn(30, sendDoorUnknownEvent)
    def hubaction = new physicalgraph.device.HubAction(
        method: "GET",
        path: "/getstatus",
        headers: [
            HOST: "${getHostAddress()}"
        ]
    )
    return hubaction
}

def parse(String description) {
    //log.trace "parse($description)"
    def msg = parseLanMessage(description)
    
    def status = msg.status          // => http status code of the response
    def data = msg.json              // => any JSON included in response body, as a data structure of lists and maps
    log.debug "Received data: ${data}"
    def result = []
    if (status == 200 || status == null) {
        //unschedule sendDoorUnknownEvent
        unschedule()
        state.nonce = data?.nonce
        if (!state.previousDoorState) {
            state.previousDoorState = device.currentValue("door")
        }
        log.debug "parse previousDoorState: ${state?.previousDoorState}"
        // don't send the event if door status haven't changed
        if (state.previousDoorState != data?.door?.status) {
            log.debug "door status has changed from '${state.previousDoorState}' to '${data?.door?.status}'. sending new event"
            result << createEvent(name: "door", value: data?.door?.status)
            state.previousDoorState = data?.door?.status
        }
        result << createEvent(name: "temperature", value: data?.temperature, unit: "F")
        result << createEvent(name: "temperatureCombined", value: data?.temperature+"° F\n"+getCelsius(data?.temperature)+"° C", displayed: false)
        result << createEvent(name: "contact", value: data?.door?.status=="closed"?"closed":"open")
    } else {
        result << createEvent(name: "door", value: "unknown")
    }
    return result
}

def sha256HashHex(text) {
    return java.security.MessageDigest.getInstance("SHA-256")
    .digest(text.getBytes("UTF-8")).encodeHex()
}

def open() {
    String pass = "${state.nonce}" + settings.password
    def secret = sha256HashHex(pass)
    log.debug secret
    def hubaction = new physicalgraph.device.HubAction(
        method: "GET",
        path: "/door/open&${secret}",
        headers: [
            HOST: "${getHostAddress()}"
        ]
    )
    return hubaction
}

def close() {
    String pass = "${state.nonce}" + settings.password
    def secret = sha256HashHex(pass)
    log.debug secret
    def hubaction = new physicalgraph.device.HubAction(
        method: "GET",
        path: "/door/close&${secret}",
        headers: [
            HOST: "${getHostAddress()}"
        ]
    )
    return hubaction
}

def getCelsius(String fahrenheit) {
    def celsius = ((Float.parseFloat(fahrenheit)-32) *  (5/9))
    return celsius.trunc(1)
}



