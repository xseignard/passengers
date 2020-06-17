const udp = require('dgram')

const client = udp.createSocket('udp4')
const msg = Buffer.from('coucou')

setInterval(() => {
  client.send(msg, 8888, '2.0.1.1', err => {
    if (err) console.log(err)
    console.log('OK')
  })
}, 16)
